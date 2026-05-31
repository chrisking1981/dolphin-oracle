// Copyright 2026 Dolphin Oracle Project (chrisking1981)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DolphinOracle.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/Socket.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/FrameDumper.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#endif

namespace DolphinOracle
{

namespace
{

struct Subscription
{
  std::uint32_t addr;
  std::uint32_t mode;       // 8 | 16 | 32
  std::uint32_t prev_value; // ~0u on first poll so we always emit first event
};

struct SubscriptionMulti
{
  std::uint32_t addr;
  std::uint32_t size;             // bytes
  std::vector<std::uint8_t> prev; // last seen bytes
};

struct Client
{
  std::unique_ptr<sf::TcpSocket> socket;
  std::string recv_buf;
  std::vector<Subscription> subs;
  std::vector<SubscriptionMulti> subs_multi;
};

// Forced-GCPad state. Per-pad hijack: client sets buttons, we keep that
// state alive for HIJACK_TIMEOUT_MS, then reset to "no controller" so the
// user can take over again.
struct ForcedGCPad
{
  std::atomic<bool> active{false};
  std::atomic<std::int64_t> expire_ms{0};
  std::mutex status_mtx;
  GCPadStatus status{};
};

static std::atomic<bool> s_running{false};
static std::thread s_thread;
static std::thread s_sub_thread;
static sf::TcpListener s_listener;
static std::mutex s_clients_mtx;
static std::vector<Client> s_clients;
static Core::System* s_system = nullptr;

static constexpr int SUB_POLL_MS = 50;       // 20 Hz subscription polling
static constexpr int HIJACK_TIMEOUT_MS = 500;
static std::array<ForcedGCPad, 4> s_forced_pads;

static std::int64_t NowMs()
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void SendLine(sf::TcpSocket& sock, const std::string& msg)
{
  std::string line = msg;
  if (line.empty() || line.back() != '\n')
    line.push_back('\n');
  // Retry-loop send: non-blocking sockets can return Partial or NotReady;
  // we must keep sending until all bytes are flushed or we hit a real error.
  std::size_t sent_total = 0;
  while (sent_total < line.size())
  {
    std::size_t sent = 0;
    auto st = sock.send(line.data() + sent_total, line.size() - sent_total, sent);
    if (st == sf::Socket::Status::Done || st == sf::Socket::Status::Partial)
    {
      sent_total += sent;
      if (st == sf::Socket::Status::Partial)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    else if (st == sf::Socket::Status::NotReady)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    else
    {
      // Disconnected or Error -- give up on this send.
      break;
    }
  }
}

static std::vector<std::string> SplitTokens(const std::string& s)
{
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok)
    out.push_back(tok);
  return out;
}

static bool ParseHex(const std::string& s, std::uint32_t& out)
{
  try
  {
    out = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
    return true;
  }
  catch (...)
  {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Command handlers -- each returns a reply line (no trailing newline).
// ---------------------------------------------------------------------------

static std::string HandleSave(const std::vector<std::string>& toks)
{
  if (toks.size() < 2)
    return "FAIL usage: SAVE <abs_path>";
  // Rejoin tokens 1..end with spaces in case the path has spaces.
  std::string path;
  for (size_t i = 1; i < toks.size(); ++i)
  {
    if (i > 1)
      path.push_back(' ');
    path += toks[i];
  }
  if (!s_system)
    return "FAIL no system";
  // Detached thread so the SAVE call doesn't block the server loop even
  // if State::SaveAs has internal synchronization. Client should poll the
  // filesystem to detect when the file appears.
  Core::System* sys = s_system;
  std::thread([sys, path]() { State::SaveAs(*sys, path, /*wait=*/true); }).detach();
  return "SUCCESS";
}

static std::string HandleLoad(const std::vector<std::string>& toks)
{
  if (toks.size() < 2)
    return "FAIL usage: LOAD <abs_path>";
  std::string path;
  for (size_t i = 1; i < toks.size(); ++i)
  {
    if (i > 1)
      path.push_back(' ');
    path += toks[i];
  }
  if (!s_system)
    return "FAIL no system";
  Core::System* sys = s_system;
  std::thread([sys, path]() { State::LoadAs(*sys, path); }).detach();
  return "SUCCESS";
}

static std::string HandleRead(const std::vector<std::string>& toks)
{
  if (toks.size() < 3)
    return "FAIL usage: READ <8|16|32> <addr_hex>";
  if (!s_system)
    return "FAIL no system";
  std::uint32_t addr = 0;
  if (!ParseHex(toks[2], addr))
    return "FAIL bad addr";
  auto& memory = s_system->GetMemory();
  // Use C locale so the OS regional setting doesn't inject thousand
  // separators (Dutch Windows turns 80003100 into 80.003.100 otherwise).
  std::ostringstream rep;
  rep.imbue(std::locale::classic());
  if (toks[1] == "8")
  {
    std::uint8_t v = memory.Read_U8(addr);
    rep << "MEM " << std::hex << addr << " " << std::hex << static_cast<unsigned>(v);
  }
  else if (toks[1] == "16")
  {
    std::uint16_t v = memory.Read_U16(addr);
    rep << "MEM " << std::hex << addr << " " << std::hex << v;
  }
  else if (toks[1] == "32")
  {
    std::uint32_t v = memory.Read_U32(addr);
    rep << "MEM " << std::hex << addr << " " << std::hex << v;
  }
  else
  {
    return "FAIL bad size (use 8|16|32)";
  }
  return rep.str();
}

static std::string HandleDumpMem(const std::vector<std::string>& toks)
{
  // DUMP_MEM <addr_hex> <size_hex> <abs_path>
  // Dump <size> guest bytes starting at <addr> straight to a file Dolphin-side
  // (no per-word TCP round trips). Used to snapshot the full 24 MB MEM1 fast.
  if (toks.size() < 4)
    return "FAIL usage: DUMP_MEM <addr_hex> <size_hex> <abs_path>";
  if (!s_system)
    return "FAIL no system";
  std::uint32_t addr = 0, size = 0;
  if (!ParseHex(toks[1], addr) || !ParseHex(toks[2], size))
    return "FAIL bad addr/size";
  if (size == 0)
    return "FAIL size is zero";
  // Path may contain spaces; rejoin tokens 3..end.
  std::string path = toks[3];
  for (std::size_t i = 4; i < toks.size(); ++i)
    path += " " + toks[i];

  auto& memory = s_system->GetMemory();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return "FAIL cannot open path: " + path;

  // Copy in modest blocks; Read_U8 honours the guest memory map.
  std::vector<std::uint8_t> buf;
  buf.reserve(0x10000);
  std::uint32_t written = 0;
  while (written < size)
  {
    const std::uint32_t block = std::min<std::uint32_t>(0x10000, size - written);
    buf.clear();
    for (std::uint32_t i = 0; i < block; ++i)
      buf.push_back(memory.Read_U8(addr + written + i));
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    written += block;
  }
  out.close();

  std::ostringstream rep;
  rep.imbue(std::locale::classic());
  rep << "SUCCESS DUMP_MEM " << std::hex << addr << " " << std::hex << size
      << " -> " << path;
  return rep.str();
}

static std::string HandleWrite(const std::vector<std::string>& toks)
{
  if (toks.size() < 4)
    return "FAIL usage: WRITE <8|16|32> <addr_hex> <val_hex>";
  if (!s_system)
    return "FAIL no system";
  std::uint32_t addr = 0, val = 0;
  if (!ParseHex(toks[2], addr) || !ParseHex(toks[3], val))
    return "FAIL bad addr/val";
  auto& memory = s_system->GetMemory();
  if (toks[1] == "8")
    memory.Write_U8(static_cast<std::uint8_t>(val), addr);
  else if (toks[1] == "16")
    memory.Write_U16(static_cast<std::uint16_t>(val), addr);
  else if (toks[1] == "32")
    memory.Write_U32(val, addr);
  else
    return "FAIL bad size (use 8|16|32)";
  return "SUCCESS";
}

static std::string HandleWriteMulti(const std::vector<std::string>& toks)
{
  // WRITE_MULTI <addr_hex> <byte_hex> [<byte_hex>...]  -- atomic-ish batch
  // write of N bytes starting at addr. All bytes go in the same dispatch
  // so the emulator sees them on the same frame.
  if (toks.size() < 3)
    return "FAIL usage: WRITE_MULTI <addr_hex> <byte_hex>...";
  if (!s_system)
    return "FAIL no system";
  std::uint32_t addr = 0;
  if (!ParseHex(toks[1], addr))
    return "FAIL bad addr";
  auto& memory = s_system->GetMemory();
  std::uint32_t cur = addr;
  std::size_t count = 0;
  for (std::size_t i = 2; i < toks.size(); ++i)
  {
    std::uint32_t v = 0;
    if (!ParseHex(toks[i], v))
      return "FAIL bad byte at idx " + std::to_string(i);
    if (v > 0xFF)
      return "FAIL byte > 0xFF at idx " + std::to_string(i);
    memory.Write_U8(static_cast<std::uint8_t>(v), cur);
    cur++;
    count++;
  }
  return "SUCCESS " + std::to_string(count);
}

// ---------------------------------------------------------------------------
// SUBSCRIBE / UNSUBSCRIBE
// ---------------------------------------------------------------------------

// NOTE: these need access to the current Client. We thread a pointer through
// the dispatcher. To keep handler signatures simple we use a thread-local-ish
// trick: stash the current client in a static during dispatch.
static thread_local Client* tl_current_client = nullptr;

static std::string HandleSubscribe(const std::vector<std::string>& toks)
{
  if (toks.size() < 3)
    return "FAIL usage: SUBSCRIBE <8|16|32> <addr_hex>";
  if (!tl_current_client)
    return "FAIL no client context";
  std::uint32_t mode = 0;
  if (toks[1] == "8") mode = 8;
  else if (toks[1] == "16") mode = 16;
  else if (toks[1] == "32") mode = 32;
  else return "FAIL bad mode (use 8|16|32)";
  std::uint32_t addr = 0;
  if (!ParseHex(toks[2], addr))
    return "FAIL bad addr";
  // Reject duplicate
  for (const auto& s : tl_current_client->subs)
    if (s.addr == addr && s.mode == mode)
      return "SUCCESS already subscribed";
  tl_current_client->subs.push_back({addr, mode, ~0u});
  return "SUCCESS";
}

static std::string HandleSubscribeMulti(const std::vector<std::string>& toks)
{
  if (toks.size() < 3)
    return "FAIL usage: SUBSCRIBE_MULTI <size_dec> <addr_hex>";
  if (!tl_current_client)
    return "FAIL no client context";
  std::uint32_t size = 0;
  try { size = static_cast<std::uint32_t>(std::stoul(toks[1])); }
  catch (...) { return "FAIL bad size"; }
  if (size == 0 || size > 4096)
    return "FAIL size out of range (1..4096)";
  std::uint32_t addr = 0;
  if (!ParseHex(toks[2], addr))
    return "FAIL bad addr";
  for (const auto& s : tl_current_client->subs_multi)
    if (s.addr == addr && s.size == size)
      return "SUCCESS already subscribed";
  SubscriptionMulti sm;
  sm.addr = addr;
  sm.size = size;
  sm.prev.assign(size, 0xFF);  // force first-poll diff
  tl_current_client->subs_multi.push_back(std::move(sm));
  return "SUCCESS";
}

static std::string HandleUnsubscribe(const std::vector<std::string>& toks)
{
  if (toks.size() < 2)
    return "FAIL usage: UNSUBSCRIBE <addr_hex>";
  if (!tl_current_client)
    return "FAIL no client context";
  std::uint32_t addr = 0;
  if (!ParseHex(toks[1], addr))
    return "FAIL bad addr";
  auto& v = tl_current_client->subs;
  auto it = std::remove_if(v.begin(), v.end(),
                           [&](const Subscription& s) { return s.addr == addr; });
  const bool removed = (it != v.end());
  v.erase(it, v.end());
  return removed ? "SUCCESS" : "SUCCESS not subscribed";
}

static std::string HandleUnsubscribeMulti(const std::vector<std::string>& toks)
{
  if (toks.size() < 2)
    return "FAIL usage: UNSUBSCRIBE_MULTI <addr_hex>";
  if (!tl_current_client)
    return "FAIL no client context";
  std::uint32_t addr = 0;
  if (!ParseHex(toks[1], addr))
    return "FAIL bad addr";
  auto& v = tl_current_client->subs_multi;
  auto it = std::remove_if(v.begin(), v.end(),
                           [&](const SubscriptionMulti& s) { return s.addr == addr; });
  const bool removed = (it != v.end());
  v.erase(it, v.end());
  return removed ? "SUCCESS" : "SUCCESS not subscribed";
}

// ---------------------------------------------------------------------------
// BUTTONSTATES_GC
// Protocol: BUTTONSTATES_GC <pad> <buttons_hex> <sx> <sy> <csx> <csy>
//   sticks are floats in [-1.0, 1.0], 0 = center
// ---------------------------------------------------------------------------

static std::string HandleButtonStatesGC(const std::vector<std::string>& toks)
{
  if (toks.size() < 7)
    return "FAIL usage: BUTTONSTATES_GC <pad> <buttons_hex> <sx> <sy> <csx> <csy>";
  int pad = 0;
  std::uint32_t buttons = 0;
  float sx = 0, sy = 0, csx = 0, csy = 0;
  try
  {
    pad = std::stoi(toks[1]);
    if (pad < 0 || pad >= 4)
      return "FAIL pad out of range (0..3)";
    if (!ParseHex(toks[2], buttons))
      return "FAIL bad buttons";
    sx = std::stof(toks[3]);
    sy = std::stof(toks[4]);
    csx = std::stof(toks[5]);
    csy = std::stof(toks[6]);
  }
  catch (...)
  {
    return "FAIL parse error";
  }
  auto clamp = [](float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); };
  sx = clamp(sx); sy = clamp(sy); csx = clamp(csx); csy = clamp(csy);

  GCPadStatus st{};
  st.button = static_cast<std::uint16_t>(buttons);
  st.stickX = static_cast<std::uint8_t>(GCPadStatus::MAIN_STICK_CENTER_X +
                                       static_cast<int>(sx * GCPadStatus::MAIN_STICK_RADIUS));
  st.stickY = static_cast<std::uint8_t>(GCPadStatus::MAIN_STICK_CENTER_Y +
                                       static_cast<int>(sy * GCPadStatus::MAIN_STICK_RADIUS));
  st.substickX = static_cast<std::uint8_t>(GCPadStatus::C_STICK_CENTER_X +
                                          static_cast<int>(csx * GCPadStatus::C_STICK_RADIUS));
  st.substickY = static_cast<std::uint8_t>(GCPadStatus::C_STICK_CENTER_Y +
                                          static_cast<int>(csy * GCPadStatus::C_STICK_RADIUS));
  st.triggerLeft = (buttons & PAD_TRIGGER_L) ? 0xFF : 0;
  st.triggerRight = (buttons & PAD_TRIGGER_R) ? 0xFF : 0;

  auto& fp = s_forced_pads[pad];
  {
    std::lock_guard lk(fp.status_mtx);
    fp.status = st;
  }
  fp.expire_ms.store(NowMs() + HIJACK_TIMEOUT_MS);
  fp.active.store(true);
  return "SUCCESS";
}

static std::string HandleSpeed(const std::vector<std::string>& toks)
{
  // SPEED <factor>  -- 0.5 = half speed, 0 = unlimited
  if (toks.size() < 2)
    return "FAIL usage: SPEED <factor>";
  try
  {
    float factor = std::stof(toks[1]);
    if (factor < 0.0f || factor > 100.0f)
      return "FAIL factor out of range [0..100]";
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, factor);
    return "SUCCESS";
  }
  catch (...)
  {
    return "FAIL bad factor (use float)";
  }
}

#ifdef _WIN32
struct WindowSearch
{
  DWORD pid = 0;
  HWND hwnd = nullptr;
};

static BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lparam)
{
  auto* search = reinterpret_cast<WindowSearch*>(lparam);
  DWORD window_pid = 0;
  GetWindowThreadProcessId(hwnd, &window_pid);
  if (window_pid != search->pid || !IsWindowVisible(hwnd))
    return TRUE;

  RECT rect{};
  if (!GetClientRect(hwnd, &rect))
    return TRUE;
  if ((rect.right - rect.left) < 64 || (rect.bottom - rect.top) < 64)
    return TRUE;

  search->hwnd = hwnd;
  return FALSE;
}

static bool GetPngEncoderClsid(CLSID* clsid)
{
  UINT count = 0;
  UINT bytes = 0;
  Gdiplus::GetImageEncodersSize(&count, &bytes);
  if (bytes == 0)
    return false;

  std::vector<unsigned char> buffer(bytes);
  auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok)
    return false;

  for (UINT i = 0; i < count; ++i)
  {
    if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0)
    {
      *clsid = encoders[i].Clsid;
      return true;
    }
  }
  return false;
}

static std::wstring WidenPath(const std::string& path)
{
  if (path.empty())
    return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (needed <= 0)
    return std::wstring(path.begin(), path.end());
  std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), needed);
  return wide;
}

static std::string CaptureWindowPng(const std::string& path)
{
  WindowSearch search;
  search.pid = GetCurrentProcessId();
  EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&search));
  if (!search.hwnd)
    return "FAIL no visible dolphin window";

  RECT client{};
  if (!GetClientRect(search.hwnd, &client))
    return "FAIL GetClientRect";
  POINT origin{0, 0};
  if (!ClientToScreen(search.hwnd, &origin))
    return "FAIL ClientToScreen";

  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0)
    return "FAIL empty client rect";

  Gdiplus::GdiplusStartupInput startup_input;
  ULONG_PTR token = 0;
  if (Gdiplus::GdiplusStartup(&token, &startup_input, nullptr) != Gdiplus::Ok)
    return "FAIL gdiplus startup";

  HDC screen_dc = GetDC(nullptr);
  if (!screen_dc)
  {
    Gdiplus::GdiplusShutdown(token);
    return "FAIL GetDC";
  }
  HDC mem_dc = CreateCompatibleDC(screen_dc);
  HBITMAP hbitmap = CreateCompatibleBitmap(screen_dc, width, height);
  if (!mem_dc || !hbitmap)
  {
    if (hbitmap)
      DeleteObject(hbitmap);
    if (mem_dc)
      DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
    Gdiplus::GdiplusShutdown(token);
    return "FAIL CreateCompatibleBitmap";
  }

  HGDIOBJ old_obj = SelectObject(mem_dc, hbitmap);
  BOOL copied = PrintWindow(search.hwnd, mem_dc, 0x2);
  if (!copied)
    copied = BitBlt(mem_dc, 0, 0, width, height, screen_dc, origin.x, origin.y, SRCCOPY);
  SelectObject(mem_dc, old_obj);
  ReleaseDC(nullptr, screen_dc);

  Gdiplus::Status status = Gdiplus::Ok;
  if (!copied)
  {
    status = Gdiplus::GenericError;
  }
  else
  {
    Gdiplus::Bitmap bitmap(hbitmap, nullptr);
    CLSID png_clsid{};
    if (!GetPngEncoderClsid(&png_clsid))
    {
      DeleteObject(hbitmap);
      DeleteDC(mem_dc);
      Gdiplus::GdiplusShutdown(token);
      return "FAIL no png encoder";
    }
    status = bitmap.Save(WidenPath(path).c_str(), &png_clsid, nullptr);
  }
  DeleteObject(hbitmap);
  DeleteDC(mem_dc);
  Gdiplus::GdiplusShutdown(token);

  if (status != Gdiplus::Ok)
    return "FAIL window png capture status " + std::to_string(static_cast<int>(status));
  return "SUCCESS";
}
#endif

static std::string HandleScreenshot(const std::vector<std::string>& toks)
{
  if (toks.size() < 2)
    return "FAIL usage: SCREENSHOT <abs_path>";
  std::string path;
  for (size_t i = 1; i < toks.size(); ++i)
  {
    if (i > 1)
      path.push_back(' ');
    path += toks[i];
  }
  // On Windows test harnesses need a deterministic file immediately. Dolphin's
  // frame dumper can accept a screenshot request yet never flush for some DOL
  // runs, so capture the visible render client directly through the oracle TCP
  // command. This keeps texture/frame dumping disabled.
#ifdef _WIN32
  const std::string window_result = CaptureWindowPng(path);
  if (window_result == "SUCCESS")
    return window_result;
#endif

  // Fallback to Dolphin's frame dumper for non-Windows or hidden-window cases.
  if (g_frame_dumper)
    g_frame_dumper->SaveScreenshot(path);
  else
    return "FAIL no framedumper";
  return "SUCCESS";
}

// Lifecycle commands -- all need active emulation.

static std::string HandlePause(const std::vector<std::string>&)
{
  if (!s_system)
    return "FAIL no system";
  if (Core::GetState(*s_system) != Core::State::Running)
    return "FAIL not running";
  Core::SetState(*s_system, Core::State::Paused);
  return "SUCCESS";
}

static std::string HandleResume(const std::vector<std::string>&)
{
  if (!s_system)
    return "FAIL no system";
  if (Core::GetState(*s_system) != Core::State::Paused)
    return "FAIL not paused";
  Core::SetState(*s_system, Core::State::Running);
  return "SUCCESS";
}

static std::string HandleReset(const std::vector<std::string>&)
{
  if (!s_system)
    return "FAIL no system";
  s_system->GetProcessorInterface().ResetButton_Tap();
  return "SUCCESS";
}

static std::string HandleStop(const std::vector<std::string>&)
{
  if (!s_system)
    return "FAIL no system";
  Core::Stop(*s_system);
  return "SUCCESS";
}

// ---------------------------------------------------------------------------
// command dispatcher
// ---------------------------------------------------------------------------

static void DispatchLine(Client& cl, const std::string& line)
{
  auto toks = SplitTokens(line);
  if (toks.empty())
    return;

  // Stash current client so SUBSCRIBE handlers can mutate its sub list.
  tl_current_client = &cl;

  std::string reply;
  const std::string& cmd = toks[0];
  if (cmd == "SAVE")
    reply = HandleSave(toks);
  else if (cmd == "LOAD")
    reply = HandleLoad(toks);
  else if (cmd == "READ")
    reply = HandleRead(toks);
  else if (cmd == "WRITE")
    reply = HandleWrite(toks);
  else if (cmd == "WRITE_MULTI")
    reply = HandleWriteMulti(toks);
  else if (cmd == "DUMP_MEM")
    reply = HandleDumpMem(toks);
  else if (cmd == "SPEED")
    reply = HandleSpeed(toks);
  else if (cmd == "SUBSCRIBE")
    reply = HandleSubscribe(toks);
  else if (cmd == "SUBSCRIBE_MULTI")
    reply = HandleSubscribeMulti(toks);
  else if (cmd == "UNSUBSCRIBE")
    reply = HandleUnsubscribe(toks);
  else if (cmd == "UNSUBSCRIBE_MULTI")
    reply = HandleUnsubscribeMulti(toks);
  else if (cmd == "BUTTONSTATES_GC")
    reply = HandleButtonStatesGC(toks);
  else if (cmd == "SCREENSHOT")
    reply = HandleScreenshot(toks);
  else if (cmd == "PAUSE")
    reply = HandlePause(toks);
  else if (cmd == "RESUME")
    reply = HandleResume(toks);
  else if (cmd == "RESET")
    reply = HandleReset(toks);
  else if (cmd == "STOP")
    reply = HandleStop(toks);
  else if (cmd == "PING")
    reply = "PONG dolphin-oracle v0.5";
  else
    reply = "FAIL unknown command: " + cmd;

  SendLine(*cl.socket, reply);
  tl_current_client = nullptr;
}

// ---------------------------------------------------------------------------
// Subscription poller thread: every 50ms scan all clients' subscriptions,
// read memory, push EVENT line on each value change. Lives for the
// duration of s_running.
// ---------------------------------------------------------------------------

static void PollerThread()
{
  Common::SetCurrentThreadName("DolphinOraclePoll");
  while (s_running.load(std::memory_order_acquire))
  {
    if (s_system)
    {
      auto& memory = s_system->GetMemory();
      std::lock_guard lk(s_clients_mtx);
      for (auto& cl : s_clients)
      {
        if (!cl.socket)
          continue;
        for (auto& sub : cl.subs)
        {
          std::uint32_t v = 0;
          if (sub.mode == 8) v = memory.Read_U8(sub.addr);
          else if (sub.mode == 16) v = memory.Read_U16(sub.addr);
          else if (sub.mode == 32) v = memory.Read_U32(sub.addr);
          if (v != sub.prev_value)
          {
            sub.prev_value = v;
            std::ostringstream msg;
            msg.imbue(std::locale::classic());
            msg << "EVENT " << std::hex << sub.addr << " " << std::dec << sub.mode
                << " " << std::hex << v;
            SendLine(*cl.socket, msg.str());
          }
        }
        for (auto& sm : cl.subs_multi)
        {
          // Read range, compare to prev.
          std::vector<std::uint8_t> cur(sm.size);
          for (std::uint32_t i = 0; i < sm.size; ++i)
            cur[i] = memory.Read_U8(sm.addr + i);
          if (cur != sm.prev)
          {
            sm.prev = cur;
            std::ostringstream msg;
            msg.imbue(std::locale::classic());
            msg << "EVENT_MULTI " << std::hex << sm.addr << " " << std::dec << sm.size
                << " " << std::hex;
            for (std::uint8_t b : cur)
              msg << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
            SendLine(*cl.socket, msg.str());
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(SUB_POLL_MS));
  }
}

// ---------------------------------------------------------------------------
// BUTTONSTATES_GC hook -- called from Pad::GetStatus() in modern Dolphin.
// We patched Pad.cpp to call this; if it returns true Pad uses our state.
// ---------------------------------------------------------------------------

}  // anonymous namespace

bool GetForcedGCPadStatus(int pad_num, GCPadStatus* out_status)
{
  if (pad_num < 0 || pad_num >= static_cast<int>(s_forced_pads.size()))
    return false;
  auto& fp = s_forced_pads[pad_num];
  if (!fp.active.load(std::memory_order_acquire))
    return false;
  if (NowMs() > fp.expire_ms.load(std::memory_order_acquire))
  {
    fp.active.store(false);
    return false;
  }
  std::lock_guard lk(fp.status_mtx);
  *out_status = fp.status;
  return true;
}

namespace
{

// ---------------------------------------------------------------------------
// server thread: poll listener + each client for new bytes
// ---------------------------------------------------------------------------

static void ServerThread(unsigned short port)
{
  Common::SetCurrentThreadName("DolphinOracle");

  s_listener.setBlocking(false);
  if (s_listener.listen(port) != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(CORE, "[DolphinOracle] could not listen on port {}", port);
    return;
  }
  NOTICE_LOG_FMT(CORE, "[DolphinOracle] listening on TCP port {}", port);

  while (s_running.load(std::memory_order_acquire))
  {
    // Accept new clients (non-blocking)
    auto incoming = std::make_unique<sf::TcpSocket>();
    incoming->setBlocking(false);
    if (s_listener.accept(*incoming) == sf::Socket::Status::Done)
    {
      NOTICE_LOG_FMT(CORE, "[DolphinOracle] client connected from {}",
                     incoming->getRemoteAddress().value_or(sf::IpAddress::Any).toString());
      Client cl{std::move(incoming), {}};
      std::lock_guard lk(s_clients_mtx);
      s_clients.push_back(std::move(cl));
    }

    // Service existing clients
    {
      std::lock_guard lk(s_clients_mtx);
      for (auto it = s_clients.begin(); it != s_clients.end();)
      {
        char buf[512];
        std::size_t got = 0;
        auto st = it->socket->receive(buf, sizeof(buf), got);
        bool drop = false;
        if (st == sf::Socket::Status::Done && got > 0)
        {
          it->recv_buf.append(buf, got);
          for (;;)
          {
            auto nl = it->recv_buf.find('\n');
            if (nl == std::string::npos)
              break;
            std::string line = it->recv_buf.substr(0, nl);
            it->recv_buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
              line.pop_back();
            if (!line.empty())
              DispatchLine(*it, line);
          }
        }
        else if (st == sf::Socket::Status::Disconnected ||
                 st == sf::Socket::Status::Error)
        {
          drop = true;
        }

        if (drop)
          it = s_clients.erase(it);
        else
          ++it;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Cleanup
  {
    std::lock_guard lk(s_clients_mtx);
    s_clients.clear();
  }
  s_listener.close();
  NOTICE_LOG_FMT(CORE, "[DolphinOracle] server stopped");
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void Init(Core::System& system)
{
  s_system = &system;
  if (s_running.load(std::memory_order_acquire))
    return;
  const unsigned short port = DEFAULT_PORT;
  s_running.store(true, std::memory_order_release);
  s_thread = std::thread(ServerThread, port);
  s_sub_thread = std::thread(PollerThread);
  NOTICE_LOG_FMT(CORE,
                 "[DolphinOracle] Oracle starting on port {} (v0.4)",
                 port);
}

void Shutdown()
{
  if (!s_running.exchange(false))
    return;
  if (s_thread.joinable())
    s_thread.join();
  if (s_sub_thread.joinable())
    s_sub_thread.join();
  s_system = nullptr;
}

}  // namespace DolphinOracle
