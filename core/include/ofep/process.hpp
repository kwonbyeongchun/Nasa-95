#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ofep {

// 외부 프로그램(솔버) 실행. 표준 출력·오류는 파일로 보낸다. 경로와 인자는 UTF-8.
// GPL 프로그램(CalculiX)은 링크하지 않고 이렇게 별도 실행 파일로만 쓴다.
class Process {
 public:
  Process();
  ~Process();  // 실행 중이면 그대로 둔다(중지는 kill 로 명시적으로)
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  // 시작하지 못하면 Error("io_error").
  void start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
             const std::map<std::string, std::string>& env, const std::string& output_file);
  bool started() const;
  bool running();
  void wait();
  void kill();
  int exit_code();  // 끝난 뒤에만 뜻이 있다

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ofep
