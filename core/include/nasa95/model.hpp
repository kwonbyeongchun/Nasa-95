#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nasa95/json.hpp"

namespace nasa95 {

using Id = std::uint64_t;

// 모델의 속성형 객체(재료·프로퍼티·케이스·스텝·하중 …).
// 대용량 데이터(메시·결과 배열, 형상)는 여기에 두지 않고 전용 저장소에 둔다.
struct Object {
  Id id = 0;
  std::string kind;
  std::string name;
  Id parent = 0;           // 0 = 최상위
  std::int64_t order = 0;  // 같은 부모·같은 종류 안에서의 순서
  bool suppressed = false;
  Json props = Json::object();

  Json to_json() const;
  static Object from_json(const Json& j);
  bool operator==(const Object& o) const;
};

// 한 객체의 변경 1건. before 없음 = 생성, after 없음 = 삭제.
struct Change {
  Id id = 0;
  std::optional<Object> before;
  std::optional<Object> after;
};
using ChangeSet = std::vector<Change>;

class Model {
 public:
  const Object* find(Id id) const;
  const Object& get(Id id) const;  // 없으면 Error("not_found")
  std::vector<const Object*> all() const;
  std::vector<const Object*> by_kind(const std::string& kind) const;  // (parent, order, id) 순
  std::vector<const Object*> children(Id parent, const std::string& kind = "") const;  // (order, id) 순
  std::size_t size() const { return objs_.size(); }

  // 변경. 기록 중이면 ChangeSet 에 남는다.
  Id create(Object o);            // id 가 0 이면 새 ID 부여
  void replace(const Object& o);  // 같은 ID 의 객체를 통째로 교체(같으면 아무 일도 없음)
  void remove(Id id);

  void begin_record();
  ChangeSet end_record();
  bool recording() const { return recording_; }

  void apply_inverse(const ChangeSet& cs);  // 되돌리기
  void apply_forward(const ChangeSet& cs);  // 다시 실행

  void clear();
  Json to_json() const;
  void load_json(const Json& j);

  // 상태 요약값(API-39). ID 발급 카운터는 포함하지 않는다.
  Json digest() const;

 private:
  void record(Change c);

  std::map<Id, Object> objs_;
  Id next_ = 1;
  bool recording_ = false;
  ChangeSet rec_;
};

}  // namespace nasa95
