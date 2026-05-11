// Copyright 2026 Dolphin Oracle Project (chrisking1981)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DolphinOracle.h"

#include <atomic>
#include <cstdint>
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
#include "VideoCommon/FrameDumper.h"

namespace DolphinOracle
{

namespace
{

struct Client
{
  std::unique_ptr<sf::TcpSocket> socket;
  std::string recv_buf;
};

static std::atomic<bool> s_running{false};
static std::thread s_thread;
static sf::TcpListener s_listener;
static std::mutex s_clients_mtx;
static std::vector<Client> s_clients;
static Core::System* s_system = nullptr;

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
  // Use the FrameDumper directly so we get the EXACT path the client asked
  // for, not Dolphin's own Screenshots/ directory. The dumper writes
  // asynchronously on the next frame_end -- client should poll filesystem.
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
    reply = "PONG dolphin-oracle v0.2";
  else
    reply = "FAIL unknown command: " + cmd;

  SendLine(*cl.socket, reply);
}

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
  NOTICE_LOG_FMT(CORE,
                 "[DolphinOracle] Oracle starting on port {} (v0.1)",
                 port);
}

void Shutdown()
{
  if (!s_running.exchange(false))
    return;
  if (s_thread.joinable())
    s_thread.join();
  s_system = nullptr;
}

}  // namespace DolphinOracle
