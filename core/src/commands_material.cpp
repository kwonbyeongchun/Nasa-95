// 재료 명령: 구성 모델 설정·제거, 검사, 곡선 조회, 라이브러리
#include <algorithm>
#include <fstream>
#include <set>

#include "nasa95/app.hpp"
#include "nasa95/error.hpp"

namespace nasa95 {

namespace {

using F = FieldSpec;

Object get_material(const Model& m, Id id) {
  const Object& o = m.get(id);
  if (o.kind != "material")
    throw Error("wrong_kind", "id=" + std::to_string(id) + " 는 material 이 아닙니다",
                {{"object", id}, {"kind", o.kind}, {"expected", "material"}});
  return o;
}

const BehaviorSpec& behavior(const std::string& name) {
  const auto& specs = material_behaviors();
  const auto it = std::find_if(specs.begin(), specs.end(), [&](const BehaviorSpec& s) { return s.name == name; });
  if (it == specs.end()) throw Error("not_found", "알 수 없는 구성 모델: " + name, {{"behavior", name}});
  return *it;
}

// `data` 표의 형식: 행마다 상수 n 개(+ 온도). 여러 온도면 온도 열이 있어야 하고 오름차순이어야 한다.
void check_data_table(const BehaviorSpec& b, const Json& params) {
  if (!b.row_size) return;
  const int n = b.row_size(params);
  const bool has = params.contains("data") && !params["data"].is_null();
  if (n == 0) {
    if (has) throw Error("unknown_param", "이 설정에서는 data 를 주지 않습니다", {{"param", "data"}});
    return;
  }
  if (!has) throw Error("missing_param", "필수 매개변수가 없습니다: data", {{"param", "data"}});
  const Json& data = params["data"];
  if (data.empty()) throw Error("invalid_param_type", "data 가 비어 있습니다", {{"param", "data"}});
  const bool with_t = static_cast<int>(data[0].size()) == n + 1;
  for (const Json& row : data)
    if (static_cast<int>(row.size()) != (with_t ? n + 1 : n))
      throw Error("invalid_param_type",
                  "data 의 행마다 값이 " + std::to_string(n) + "개(온도 의존이면 " + std::to_string(n + 1) + "개)여야 합니다",
                  {{"param", "data"}, {"expected_columns", n}});
  if (!b.curve && !with_t && data.size() > 1)
    throw Error("invalid_param_type", "행이 여러 개면 행마다 끝에 온도를 붙여야 합니다", {{"param", "data"}});
  for (std::size_t i = 1; i < data.size(); ++i) {
    const double t0 = with_t ? data[i - 1][n].get<double>() : 0.0;
    const double t1 = with_t ? data[i][n].get<double>() : 0.0;
    const bool same_t = !with_t || t0 == t1;
    bool ok = with_t ? t1 >= t0 : true;
    if (!b.curve && with_t) ok = t1 > t0;
    if (b.curve && same_t) ok = data[i][1].get<double>() > data[i - 1][1].get<double>();
    if (!ok)
      throw Error("invalid_order", "data 의 온도(곡선은 같은 온도 안의 둘째 열)는 오름차순이어야 합니다",
                  {{"param", "data"}, {"row", i}});
  }
}

Json validated_behavior(const App& a, const BehaviorSpec& b, const Json& p) {
  Json data = Json::object();
  for (const FieldSpec& f : b.fields) {
    if (!p.contains(f.name) || p[f.name].is_null()) continue;
    check_value(f, p[f.name], &a.model());
    data[f.name] = p[f.name];
  }
  check_data_table(b, data);
  return data;
}

// 선형 보간. 범위 밖은 끝 값.
double interpolate(const std::vector<std::pair<double, double>>& pts, double x) {
  if (x <= pts.front().first) return pts.front().second;
  if (x >= pts.back().first) return pts.back().second;
  for (std::size_t i = 1; i < pts.size(); ++i)
    if (x <= pts[i].first) {
      const double t = (x - pts[i - 1].first) / (pts[i].first - pts[i - 1].first);
      return pts[i - 1].second + t * (pts[i].second - pts[i - 1].second);
    }
  return pts.back().second;
}

std::string unique_material_name(const App& a, const std::string& base) {
  std::set<std::string> used;
  for (const Object* o : a.model().by_kind("material")) used.insert(o->name);
  if (!used.count(base)) return base;
  for (int n = 2;; ++n) {
    std::string name = base + "-" + std::to_string(n);
    if (!used.count(name)) return name;
  }
}

// 내장 재료 DB(MAT-11): 자주 쓰는 재료의 공칭값. 단위계 m-kg-s(Pa, kg/m³, W/(m·K), J/(kg·K)), 온도 K 기준 20 °C 값.
// 출처: 제조사·핸드북의 대표값(설계 검토에는 규격·시험 성적서 값을 쓸 것). 가져올 때 모델 단위계로 환산한다
Json builtin_library() {
  static const Json lib = Json::parse(R"json({
   "format": "open-fep-materials", "version": 1, "unit_system": "m-kg-s", "builtin": true,
   "materials": [
    {"name": "Steel S235JR", "group": "강재", "note": "일반 구조용 탄소강(SS275 상당). 항복 235 MPa, 인장 360~510 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[210e9, 0.30]]}, "density": {"data": [[7850]]},
      "expansion": {"type": "iso", "data": [[1.2e-5]]}, "conductivity": {"type": "iso", "data": [[45]]}, "specific_heat": {"data": [[460]]},
      "plastic": {"hardening": "isotropic", "data": [[235e6, 0.0], [360e6, 0.20]]}}}},
    {"name": "Steel S355J2", "group": "강재", "note": "고장력 구조용강(SM355 상당). 항복 355 MPa, 인장 470~630 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[210e9, 0.30]]}, "density": {"data": [[7850]]},
      "expansion": {"type": "iso", "data": [[1.2e-5]]}, "conductivity": {"type": "iso", "data": [[45]]}, "specific_heat": {"data": [[460]]},
      "plastic": {"hardening": "isotropic", "data": [[355e6, 0.0], [510e6, 0.18]]}}}},
    {"name": "Steel AISI 1045", "group": "강재", "note": "기계 구조용 중탄소강(SM45C 상당), 노멀라이징",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[205e9, 0.29]]}, "density": {"data": [[7850]]},
      "expansion": {"type": "iso", "data": [[1.15e-5]]}, "conductivity": {"type": "iso", "data": [[49.8]]}, "specific_heat": {"data": [[486]]},
      "plastic": {"hardening": "isotropic", "data": [[450e6, 0.0], [700e6, 0.12]]}}}},
    {"name": "Stainless 304", "group": "강재", "note": "오스테나이트 스테인리스강(STS304)",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[193e9, 0.29]]}, "density": {"data": [[8000]]},
      "expansion": {"type": "iso", "data": [[1.73e-5]]}, "conductivity": {"type": "iso", "data": [[16.2]]}, "specific_heat": {"data": [[500]]},
      "plastic": {"hardening": "isotropic", "data": [[215e6, 0.0], [505e6, 0.40]]}}}},
    {"name": "Gray cast iron GG25", "group": "강재", "note": "회주철(GC250). 인장 250 MPa, 소성 없음(취성)",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[110e9, 0.26]]}, "density": {"data": [[7200]]},
      "expansion": {"type": "iso", "data": [[1.1e-5]]}, "conductivity": {"type": "iso", "data": [[46]]}, "specific_heat": {"data": [[490]]}}}},
    {"name": "Aluminum 6061-T6", "group": "비철", "note": "범용 알루미늄 합금. 항복 276 MPa, 인장 310 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[68.9e9, 0.33]]}, "density": {"data": [[2700]]},
      "expansion": {"type": "iso", "data": [[2.36e-5]]}, "conductivity": {"type": "iso", "data": [[167]]}, "specific_heat": {"data": [[896]]},
      "plastic": {"hardening": "isotropic", "data": [[276e6, 0.0], [310e6, 0.10]]}}}},
    {"name": "Aluminum 7075-T6", "group": "비철", "note": "고강도 알루미늄(항공). 항복 503 MPa, 인장 572 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[71.7e9, 0.33]]}, "density": {"data": [[2810]]},
      "expansion": {"type": "iso", "data": [[2.36e-5]]}, "conductivity": {"type": "iso", "data": [[130]]}, "specific_heat": {"data": [[960]]},
      "plastic": {"hardening": "isotropic", "data": [[503e6, 0.0], [572e6, 0.08]]}}}},
    {"name": "Titanium Ti-6Al-4V", "group": "비철", "note": "Grade 5 티타늄 합금. 항복 880 MPa, 인장 950 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[113.8e9, 0.342]]}, "density": {"data": [[4430]]},
      "expansion": {"type": "iso", "data": [[8.6e-6]]}, "conductivity": {"type": "iso", "data": [[6.7]]}, "specific_heat": {"data": [[526]]},
      "plastic": {"hardening": "isotropic", "data": [[880e6, 0.0], [950e6, 0.10]]}}}},
    {"name": "Copper C11000", "group": "비철", "note": "전기동(어닐링). 항복 70 MPa, 인장 220 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[117e9, 0.34]]}, "density": {"data": [[8940]]},
      "expansion": {"type": "iso", "data": [[1.7e-5]]}, "conductivity": {"type": "iso", "data": [[391]]}, "specific_heat": {"data": [[385]]},
      "plastic": {"hardening": "isotropic", "data": [[70e6, 0.0], [220e6, 0.40]]}}}},
    {"name": "Brass C26000", "group": "비철", "note": "황동(70/30), 반경질",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[110e9, 0.34]]}, "density": {"data": [[8530]]},
      "expansion": {"type": "iso", "data": [[2.0e-5]]}, "conductivity": {"type": "iso", "data": [[120]]}, "specific_heat": {"data": [[380]]},
      "plastic": {"hardening": "isotropic", "data": [[300e6, 0.0], [430e6, 0.20]]}}}},
    {"name": "Concrete C30/37", "group": "토목", "note": "보통 콘크리트 fck 30 MPa. 선형 탄성만(균열·압괴는 별도 모델)",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[33e9, 0.20]]}, "density": {"data": [[2400]]},
      "expansion": {"type": "iso", "data": [[1.0e-5]]}, "conductivity": {"type": "iso", "data": [[1.7]]}, "specific_heat": {"data": [[880]]}}}},
    {"name": "Glass soda-lime", "group": "세라믹", "note": "판유리. 취성",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[72e9, 0.22]]}, "density": {"data": [[2500]]},
      "expansion": {"type": "iso", "data": [[9.0e-6]]}, "conductivity": {"type": "iso", "data": [[1.0]]}, "specific_heat": {"data": [[840]]}}}},
    {"name": "ABS", "group": "플라스틱", "note": "범용 ABS. 항복 ≈ 40 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[2.3e9, 0.35]]}, "density": {"data": [[1040]]},
      "expansion": {"type": "iso", "data": [[9.0e-5]]}, "conductivity": {"type": "iso", "data": [[0.25]]}, "specific_heat": {"data": [[1400]]},
      "plastic": {"hardening": "isotropic", "data": [[40e6, 0.0], [45e6, 0.05]]}}}},
    {"name": "Nylon PA66", "group": "플라스틱", "note": "건조 상태. 항복 ≈ 80 MPa",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[2.9e9, 0.39]]}, "density": {"data": [[1140]]},
      "expansion": {"type": "iso", "data": [[8.0e-5]]}, "conductivity": {"type": "iso", "data": [[0.25]]}, "specific_heat": {"data": [[1700]]},
      "plastic": {"hardening": "isotropic", "data": [[80e6, 0.0], [85e6, 0.10]]}}}},
    {"name": "Rubber (linear)", "group": "플라스틱", "note": "선형 근사(ν 0.49). 큰 변형은 초탄성 모델을 쓸 것",
     "props": {"behaviors": {"elastic": {"type": "iso", "data": [[5e6, 0.49]]}, "density": {"data": [[1100]]},
      "expansion": {"type": "iso", "data": [[2.0e-4]]}, "conductivity": {"type": "iso", "data": [[0.16]]}, "specific_heat": {"data": [[1800]]}}}}
   ]})json");
  return lib;
}

Json read_library(const std::string& path) {
  if (path.empty()) return builtin_library();
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Error("io_error", "파일을 열 수 없습니다: " + path, {{"path", path}});
  Json j;
  try {
    j = Json::parse(f);
  } catch (const Json::exception&) {
    throw Error("invalid_file", "재료 라이브러리 파일을 읽을 수 없습니다", {{"path", path}});
  }
  if (!j.is_object() || j.value("format", std::string()) != "open-fep-materials" || !j.contains("materials"))
    throw Error("invalid_file", "재료 라이브러리 파일이 아닙니다", {{"path", path}});
  return j;
}

}  // namespace

void register_material_commands(App& app) {
  for (const BehaviorSpec& b : material_behaviors()) {
    const std::string name = b.name;
    {
      CommandSpec c;
      c.name = "material.set_" + name;
      c.kind = 'C', c.undoable = true, c.target = "material", c.features = b.features;
      c.desc = "재료의 구성 모델 " + name + "(" + b.label + ") 설정";
      c.params = {F("id", "ref", "재료").call_req()};
      c.params.insert(c.params.end(), b.fields.begin(), b.fields.end());
      c.fn = [name](App& a, const Json& p) {
        Object o = get_material(a.model(), p["id"].get<Id>());
        o.props["behaviors"][name] = validated_behavior(a, behavior(name), p);  // 통째로 교체한다
        a.model().replace(o);
        return Json{{"id", o.id}};
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c;
      c.name = "material.remove_" + name;
      c.kind = 'C', c.undoable = true, c.target = "material", c.features = "MAT-01";
      c.desc = "재료의 구성 모델 " + name + "(" + b.label + ") 제거";
      c.params = {F("id", "ref", "재료").call_req()};
      c.fn = [name](App& a, const Json& p) {
        Object o = get_material(a.model(), p["id"].get<Id>());
        auto bit = o.props.find("behaviors");
        if (bit == o.props.end() || !bit->contains(name))
          throw Error("not_found", "이 재료에는 구성 모델 " + name + " 이 없습니다",
                      {{"object", o.id}, {"behavior", name}});
        bit->erase(name);
        a.model().replace(o);
        return Json{{"id", o.id}};
      };
      app.register_command(std::move(c));
    }
  }

  {
    CommandSpec c;
    c.name = "material.set_orientation", c.kind = 'C', c.undoable = true, c.target = "material";
    c.features = "MAT-10", c.desc = "재료 방향을 지정한다";
    c.params = {F("id", "ref", "재료").call_req(), F("orientation", "ref", "방향(null 이면 해제)").ref("orientation")};
    c.fn = [](App& a, const Json& p) {
      Object o = get_material(a.model(), p["id"].get<Id>());
      if (!p.contains("orientation") || p["orientation"].is_null()) {
        o.props.erase("orientation");
      } else {
        check_value(F("orientation", "ref", "").ref("orientation"), p["orientation"], &a.model());
        o.props["orientation"] = p["orientation"];
      }
      a.model().replace(o);
      return Json{{"id", o.id}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = "material.check", c.kind = 'Q', c.target = "material", c.features = "MAT-12, MAT-28";
    c.desc = "필수 물성 누락·범위 위반·조합 오류를 조회한다";
    c.params = {F("id", "ref", "재료").call_req()};
    c.fn = [](App& a, const Json& p) { return diagnose_object(a, get_material(a.model(), p["id"].get<Id>())); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = "material.curve", c.kind = 'Q', c.target = "material", c.features = "MAT-04, MAT-07";
    c.desc = "물성 곡선(경화 곡선, 온도 의존 표)을 조회하고 지정 온도의 값을 보간한다";
    c.params = {F("id", "ref", "재료").call_req(), F("behavior", "string", "구성 모델 이름").call_req().ex("elastic"),
                F("temperature", "number", "이 온도에서의 상수를 보간한다")};
    c.fn = [](App& a, const Json& p) {
      const Object o = get_material(a.model(), p["id"].get<Id>());
      const std::string name = p["behavior"].get<std::string>();
      const BehaviorSpec& b = behavior(name);
      auto bit = o.props.find("behaviors");
      if (bit == o.props.end() || !bit->contains(name) || !(*bit)[name].contains("data"))
        throw Error("not_found", "이 재료에는 구성 모델 " + name + " 의 표가 없습니다",
                    {{"object", o.id}, {"behavior", name}});
      const Json& params = (*bit)[name];
      const Json& data = params["data"];
      Json out{{"behavior", name}, {"data", data}};
      if (p.contains("temperature") && b.row_size && !b.curve) {
        const int n = b.row_size(params);
        Json values = Json::array();
        const bool with_t = static_cast<int>(data[0].size()) == n + 1;
        for (int col = 0; col < n; ++col) {
          if (!with_t) {
            values.push_back(data[0][col]);
            continue;
          }
          std::vector<std::pair<double, double>> pts;
          for (const Json& row : data) pts.emplace_back(row[n].get<double>(), row[col].get<double>());
          values.push_back(interpolate(pts, p["temperature"].get<double>()));
        }
        out["values"] = values;
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = "material.library_export", c.kind = 'S', c.target = "material", c.features = "MAT-11";
    c.desc = "재료를 라이브러리 파일로 저장한다";
    c.params = {F("path", "string", "라이브러리 파일").call_req().ex("materials.json"),
                F("ids", "ref_list", "저장할 재료(없으면 전부)").ref("material")};
    c.fn = [](App& a, const Json& p) {
      Json mats = Json::array();
      auto put = [&](const Object& o) {
        Json props = o.props;
        props.erase("orientation");  // 다른 프로젝트의 객체를 가리킬 수 없다
        mats.push_back(Json{{"name", o.name}, {"props", props}});
      };
      if (p.contains("ids") && !p["ids"].is_null()) {
        for (const Json& id : p["ids"]) put(get_material(a.model(), id.get<Id>()));
      } else {
        for (const Object* o : a.model().by_kind("material")) put(*o);
      }
      const std::string path = p["path"].get<std::string>();
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (!f) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
      f << Json{{"format", "open-fep-materials"}, {"version", 1}, {"materials", mats}}.dump(1);
      return Json{{"count", mats.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = "material.library_list", c.kind = 'Q', c.target = "material", c.features = "MAT-11";
    c.desc = "재료 라이브러리의 재료 목록을 조회한다(path 가 없으면 내장 재료 DB). 항목마다 이름·구성 모델·분류·설명과 대표값(E, ν, 밀도 — 모델 단위계로 환산)";
    c.params = {F("path", "string", "라이브러리 파일(없으면 내장 DB)").ex("materials.json")};
    c.fn = [](App& a, const Json& p) {
      Json out = Json::array();
      const Json lib = read_library(p.value("path", std::string()));  // 임시 객체를 훑지 않도록 먼저 잡아 둔다
      const Object* st = a.find_settings();
      const std::string model_units = st ? st->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
      const std::string lib_units = lib.value("unit_system", model_units);
      for (const Json& m : lib["materials"]) {
        Json behaviors = Json::array();
        Json converted = m["props"].value("behaviors", Json::object());
        convert_material_behaviors(converted, lib_units, model_units);
        for (auto it = converted.begin(); it != converted.end(); ++it) behaviors.push_back(it.key());
        Json row{{"name", m["name"]}, {"behaviors", behaviors}, {"group", m.value("group", std::string())}, {"note", m.value("note", std::string())},
                 {"unit_system", model_units}};
        if (converted.contains("elastic") && converted["elastic"].value("type", std::string("iso")) == "iso" && !converted["elastic"]["data"].empty())
          row["E"] = converted["elastic"]["data"][0][0], row["nu"] = converted["elastic"]["data"][0][1];
        if (converted.contains("density") && !converted["density"]["data"].empty()) row["density"] = converted["density"]["data"][0][0];
        if (converted.contains("plastic") && converted["plastic"].contains("data") && !converted["plastic"]["data"].empty()) row["yield"] = converted["plastic"]["data"][0][0];
        out.push_back(row);
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = "material.library_import", c.kind = 'C', c.undoable = true, c.target = "material";
    c.features = "MAT-11", c.desc = "라이브러리(path 가 없으면 내장 재료 DB)에서 재료를 가져온다(이름이 겹치면 번호를 붙인다). 라이브러리의 unit_system 이 모델과 다르면 값을 모델 단위계로 환산한다";
    c.params = {F("path", "string", "라이브러리 파일(없으면 내장 DB)").ex("materials.json"),
                F("names", "string_list", "가져올 재료 이름(없으면 전부)")};
    c.fn = [](App& a, const Json& p) {
      const Json lib = read_library(p.value("path", std::string()));
      const Object* st = a.find_settings();
      const std::string model_units = st ? st->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
      const std::string lib_units = lib.value("unit_system", model_units);
      if (!known_unit_system(lib_units)) throw Error("invalid_file", "라이브러리의 단위계를 모릅니다: " + lib_units, {{"unit_system", lib_units}});
      std::set<std::string> want;
      if (p.contains("names") && !p["names"].is_null())
        for (const Json& n : p["names"]) want.insert(n.get<std::string>());
      const bool filter = !want.empty();
      Json created = Json::array();
      std::int64_t order = 0;
      for (const Object* o : a.model().by_kind("material")) order = std::max(order, o->order + 1);
      for (const Json& m : lib["materials"]) {
        const std::string name = m.at("name").get<std::string>();
        if (filter && !want.count(name)) continue;
        want.erase(name);  // 남는 이름은 라이브러리에 없는 것이다
        Object o;
        o.kind = "material";
        o.name = unique_material_name(a, name);
        o.order = order++;
        // 파일 내용을 그대로 믿지 않고 구성 모델마다 다시 검증한다.
        Json behaviors = Json::object();
        Json source = m.at("props").value("behaviors", Json::object());
        convert_material_behaviors(source, lib_units, model_units);  // 라이브러리 단위계 → 모델 단위계
        for (auto it = source.begin(); it != source.end(); ++it)
          behaviors[it.key()] = validated_behavior(a, behavior(it.key()), it.value());
        if (!behaviors.empty()) o.props["behaviors"] = behaviors;
        if (m["props"].contains("description")) o.props["description"] = m["props"]["description"];
        else if (m.contains("note")) o.props["description"] = m["note"];  // 내장 DB 의 설명(출처·주의)을 남긴다
        created.push_back(Json{{"id", a.model().create(o)}, {"name", o.name}});
      }
      if (!want.empty())
        throw Error("not_found", "라이브러리에 없는 재료: " + *want.begin(), {{"param", "names"}, {"name", *want.begin()}});
      return Json{{"created", created}};
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
