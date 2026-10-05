#include "nasa95/process.hpp"

#include "nasa95/error.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace nasa95 {

#ifdef _WIN32

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// 명령줄 인자 하나를 따옴표 규칙에 맞게 붙인다.
void append_arg(std::wstring& cmd, const std::wstring& arg) {
  if (!cmd.empty()) cmd += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd += L'"';
  std::size_t backslashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
    } else if (ch == L'"') {
      cmd.append(backslashes * 2 + 1, L'\\');
      backslashes = 0;
      cmd += ch;
      continue;
    } else {
      backslashes = 0;
    }
    cmd += ch;
  }
  cmd.append(backslashes, L'\\');
  cmd += L'"';
}

}  // namespace

struct Process::Impl {
  HANDLE process = nullptr;
  HANDLE job = nullptr;  // 자식이 띄운 프로세스까지 함께 끝내기 위해
  DWORD code = 0;
  bool done = false;
  ~Impl() {
    if (process) CloseHandle(process);
    if (job) CloseHandle(job);
  }
};

Process::Process() : impl_(new Impl) {}
Process::~Process() = default;

void Process::start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
                    const std::map<std::string, std::string>& env, const std::string& output_file) {
  std::wstring cmd;
  append_arg(cmd, wide(exe));
  for (const std::string& a : args) append_arg(cmd, wide(a));

  // 환경: 현재 환경에 덧붙인다.
  std::map<std::wstring, std::wstring> vars;
  if (LPWCH block = GetEnvironmentStringsW()) {
    for (LPWCH p = block; *p; p += wcslen(p) + 1) {
      const std::wstring entry(p);
      const std::size_t eq = entry.find(L'=', 1);
      if (eq != std::wstring::npos) vars[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [k, v] : env) vars[wide(k)] = wide(v);
  std::wstring env_block;
  for (const auto& [k, v] : vars) env_block += k + L"=" + v + L'\0';
  env_block += L'\0';

  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE out = CreateFileW(wide(output_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) throw Error("io_error", "출력 파일을 만들 수 없습니다: " + output_file, {{"path", output_file}});
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul, si.hStdOutput = out, si.hStdError = out;
  PROCESS_INFORMATION pi{};
  const std::wstring wcwd = wide(cwd);
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env_block.data(),
                                 wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  const DWORD err = GetLastError();
  CloseHandle(out);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) throw Error("io_error", "프로그램을 시작할 수 없습니다: " + exe, {{"path", exe}, {"os_error", static_cast<int>(err)}});
  impl_->job = CreateJobObjectW(nullptr, nullptr);
  if (impl_->job) AssignProcessToJobObject(impl_->job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  impl_->process = pi.hProcess;
  impl_->done = false;
}

bool Process::started() const { return impl_->process != nullptr; }

bool Process::running() {
  if (!impl_->process || impl_->done) return false;
  if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT) return true;
  GetExitCodeProcess(impl_->process, &impl_->code);
  impl_->done = true;
  return false;
}

void Process::wait() {
  if (!impl_->process || impl_->done) return;
  WaitForSingleObject(impl_->process, INFINITE);
  GetExitCodeProcess(impl_->process, &impl_->code);
  impl_->done = true;
}

void Process::kill() {
  if (!running()) return;
  if (impl_->job) TerminateJobObject(impl_->job, 1);
  else TerminateProcess(impl_->process, 1);
  wait();
}

int Process::exit_code() { return running() ? 0 : static_cast<int>(impl_->code); }

#else  // POSIX

struct Process::Impl {
  pid_t pid = -1;
  int code = 0;
  bool done = false;
};

Process::Process() : impl_(new Impl) {}
Process::~Process() = default;

void Process::start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
                    const std::map<std::string, std::string>& env, const std::string& output_file) {
  const pid_t pid = fork();
  if (pid < 0) throw Error("io_error", "프로그램을 시작할 수 없습니다: " + exe, {{"path", exe}});
  if (pid == 0) {
    setpgid(0, 0);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    const int fd = open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) dup2(fd, 1), dup2(fd, 2);
    for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(exe.c_str(), argv.data());
    _exit(127);
  }
  impl_->pid = pid, impl_->done = false;
}

bool Process::started() const { return impl_->pid > 0; }

bool Process::running() {
  if (impl_->pid <= 0 || impl_->done) return false;
  int status = 0;
  if (waitpid(impl_->pid, &status, WNOHANG) == 0) return true;
  impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  impl_->done = true;
  return false;
}

void Process::wait() {
  if (impl_->pid <= 0 || impl_->done) return;
  int status = 0;
  waitpid(impl_->pid, &status, 0);
  impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  impl_->done = true;
}

void Process::kill() {
  if (!running()) return;
  ::kill(-impl_->pid, SIGKILL);
  wait();
}

int Process::exit_code() { return running() ? 0 : impl_->code; }

#endif

}  // namespace nasa95
