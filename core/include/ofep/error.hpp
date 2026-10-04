#pragma once
#include <stdexcept>
#include <string>
#include <utility>

#include "ofep/json.hpp"

namespace ofep {

// 구조화된 오류(API-05). code 는 기계 판독용, details 에는 command·param·object 등을 담는다.
class Error : public std::runtime_error {
 public:
  Error(std::string code, const std::string& message, Json details = Json::object())
      : std::runtime_error(message), code_(std::move(code)), details_(std::move(details)) {}

  const std::string& code() const { return code_; }
  const Json& details() const { return details_; }
  Json& details() { return details_; }

  Json to_json() const {
    return Json{{"code", code_}, {"message", what()}, {"details", details_}};
  }

 private:
  std::string code_;
  Json details_;
};

}  // namespace ofep
