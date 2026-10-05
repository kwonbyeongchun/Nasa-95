// 형상: 파트와 피처(GEO-03, GEO-39).
// 파트의 형상은 피처를 순서대로 적용한 결과다. 형상 자체는 객체에 저장하지 않고 피처에서 다시 계산한다
// (그래서 Undo·저장·열기가 피처 객체만으로 끝난다).
#include <set>

#include "kinds.hpp"
#include "nasa95/error.hpp"

namespace nasa95::kinds {

namespace {

F point(const std::string& name, const std::string& desc) { return F(name, "vector3", desc).unit("length").ex({0.0, 0.0, 0.0}); }
F axis(const std::string& name, const std::string& desc) { return F(name, "vector3", desc).ex({0.0, 0.0, 1.0}); }
F tools(bool required) {
  F f = F("tools", "ref_list", "함께 쓸 다른 파트").ref("part");
  return required ? f.req() : f;
}
// 모서리·면 번호 목록. 번호는 이 피처 바로 앞까지의 형상에서 센다(geometry.entities 를 그 피처로 rollback 해서 본 번호).
F entities(const std::string& name, const std::string& desc) { return F(name, "integer_list", desc + "(앞 피처까지의 형상 기준)").req().ex({1}); }
F profile_face() { return F("face", "integer", "단면이 되는 면 번호(앞 피처까지의 형상 기준). points·sketch 와 셋 중 하나").ge(1).ex(1); }
F profile_sketch() { return F("sketch", "ref", "단면이 되는 스케치(닫힌 영역 profile 번째, 기본 1)").ref("sketch"); }
F profile_index() { return F("profile", "integer", "sketch 의 닫힌 영역 번호(sketch.profiles 순, 기본 1)").ge(1).ex(1); }
F profile_points() {
  return F("points", "table", "단면이 되는 닫힌 평면 다각형의 꼭짓점(순서대로). face 와 둘 중 하나").columns(3).unit("length")
      .ex(rows({{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 5.0, 0.0}, {0.0, 5.0, 0.0}}));
}
F datum(const char* desc) { return F("datum", "ref", desc).ref("datum"); }
// 불리언의 퍼지 공차(OCCT fuzzy value): 이 거리 안의 거의 붙은 면·틈·겹침을 같은 것으로 본다. 없으면 정확 연산
F fuzzy() { return F("tolerance", "number", "퍼지 공차: 이 거리 안의 거의 붙은 면·틈을 같은 것으로 보고 연산한다(없으면 정확히)").gt(0).unit("length").ex(1e-4); }
F merge() { return F("merge", "string", "만든 바디를 앞 형상과 합치는 방법: none = 따로 둠, fuse = 합침, cut = 뺌").one_of({"none", "fuse", "cut"}); }

}  // namespace

void register_geometry(Schema& s) {
  {
    KindSpec k;
    k.kind = "part", k.label = "파트", k.collection = "parts";
    k.features = "GEO-03, WT-06";
    k.fields = {F("description", "string", "설명").ex("브래킷"),
                F("rollback", "ref", "이 피처까지만 적용한다(없으면 끝까지)").ref("feature"),
                F("assembly", "ref", "상위 어셈블리(이 파트가 든 그룹 파트). 트리에서 그 아래에 나온다").ref("part"),
                F("color", "number_list", "표시 색 [r, g, b] (0~1). STEP 으로 왕복한다").ge(0).le(1).ex({0.8, 0.2, 0.2})};
    k.check = [](const Model& m, const Object& o) {
      if (o.props.contains("color") && o.props["color"].is_array() && o.props["color"].size() != 3)
        throw Error("invalid_param_type", "색은 [r, g, b] 세 값이어야 합니다", {{"param", "color"}, {"object", o.id}});
      // 어셈블리 순환 금지
      std::set<Id> seen{o.id};
      Id cur = o.props.contains("assembly") && o.props["assembly"].is_number() ? o.props["assembly"].get<Id>() : 0;
      while (cur) {
        if (!seen.insert(cur).second) throw Error("cyclic_dependency", "어셈블리 계층이 순환합니다", {{"param", "assembly"}, {"object", o.id}});
        const Object* up = m.find(cur);
        cur = up && up->props.contains("assembly") && up->props["assembly"].is_number() ? up->props["assembly"].get<Id>() : 0;
      }
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 기준 형상(GEO-19): 분할·대칭·회전·좌표계의 기준으로 쓰는 점·축·평면
    k.kind = "datum", k.label = "기준 형상", k.collection = "datums";
    k.features = "GEO-19";
    k.subtypes = {{"point", {point("point", "점").req()}},
                  {"axis", {point("point", "축 위의 점"), axis("direction", "축 방향").req()}},
                  {"plane", {point("point", "평면 위의 점"), axis("normal", "평면의 법선").req()}}};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 스케치(GEO-32~38): 평면 위의 2D 요소 목록과 구속 목록(해석기는 sketch_solver.cpp, D8 자체 구현)
    k.kind = "sketch", k.label = "스케치", k.collection = "sketches";
    k.features = "GEO-32, GEO-33, GEO-34, GEO-35, GEO-36, GEO-38";
    k.parents = {"part"};
    k.fields = {point("point", "스케치 평면의 원점"), axis("normal", "평면의 법선(기본 z)"), axis("x_axis", "스케치 u 축 방향(없으면 법선에서 정한다)"),
                F("datum", "ref", "기준 평면(점·법선 대신)").ref("datum"),
                F("entities", "object_list", "요소 목록(sketch.add_* 로 더한다): {id, kind, 좌표…} — 좌표는 스케치 평면의 (u, v)")
                    .of({F("id", "integer", "요소 번호").call_req().ex(1), F("kind", "string", "종류").call_req().one_of({"line", "circle", "arc", "rectangle", "ellipse", "spline", "point"}).ex("line"),
                         F("start", "number_list", "line·arc: 시작점 [u, v]").ex({0.0, 0.0}), F("end", "number_list", "line·arc: 끝점 [u, v]").ex({10.0, 0.0}),
                         F("middle", "number_list", "arc: 호 위의 점 [u, v]").ex({5.0, 5.0}), F("center", "number_list", "circle·ellipse: 중심 [u, v]").ex({0.0, 0.0}),
                         F("radius", "number", "circle: 반지름").gt(0).ex(5.0), F("radii", "number_list", "ellipse: [큰 반지름, 작은 반지름]").ex({5.0, 3.0}),
                         F("angle", "number", "ellipse: 큰 축의 각도(도)").ex(0.0), F("corner", "number_list", "rectangle: 한 꼭짓점 [u, v]").ex({0.0, 0.0}),
                         F("size", "number_list", "rectangle: [너비, 높이]").ex({10.0, 5.0}), F("points", "table", "spline: 지나는 점 [[u, v], …]").columns(2).ex(rows({{0.0, 0.0}, {5.0, 3.0}})),
                         F("position", "number_list", "point: [u, v]").ex({1.0, 1.0}), F("reference", "bool", "참조 요소(투영한 것: 단면에 쓰지 않음)")}),
                F("constraints", "object_list", "구속 목록(sketch.add_constraint / add_dimension 으로 더한다)")
                    .of({F("id", "integer", "구속 번호").call_req().ex(1),
                         F("kind", "string", "종류").call_req()
                             .one_of({"coincident", "horizontal", "vertical", "parallel", "perpendicular", "tangent", "equal", "concentric", "fixed", "symmetric",
                                      "point_on_line", "point_on_circle", "length", "distance", "horizontal_distance", "vertical_distance", "angle", "radius", "diameter"})
                             .ex("horizontal"),
                         F("entities", "integer_list", "대상 요소 번호").ex(ints({1})),
                         F("points", "object_list", "대상 점 [{entity, point: start|end|center|middle|position}, …]")
                             .of({F("entity", "integer", "요소 번호").call_req().ge(1).ex(1), F("point", "string", "어느 점(점·원·사각형 요소는 생략 가능)").ex("end")}),
                         F("value", "number", "치수 값(길이·거리·각도(도)·반지름·지름)").ex(50.0),
                         F("at", "number_list", "fixed: 고정할 좌표 [u, v](없으면 지금 자리)").ex({0.0, 0.0}),
                         F("internal", "bool", "tangent(원-원): 안쪽 접촉")})};
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "feature", k.label = "피처", k.collection = "features";
    k.features = "GEO-39";
    k.parents = {"part"};
    k.subtypes = {
        // --- 형상을 더하는 피처: 파트에 바디 하나를 더한다
        {"import",  // 가져온 형상(이력의 시작). 형상은 BREP 글로 품는다
         {F("brep", "string", "형상(BREP 형식의 글)").call_req().ex("..."), F("source", "string", "가져온 파일").ex("bracket.step"),
          F("format", "string", "원본 형식").one_of({"step", "iges", "brep"}),
          F("scale", "number", "배율(원본 단위 → 모델 단위)").gt(0)}},
        {"box", {point("origin", "한 꼭짓점"), F("size", "vector3", "x, y, z 크기").req().unit("length").ex({10.0, 20.0, 30.0})}},
        {"cylinder",
         {point("origin", "밑면 중심"), axis("axis", "축 방향"), F("radius", "number", "반지름").req().gt(0).unit("length").ex(5.0),
          F("height", "number", "높이").req().gt(0).unit("length").ex(20.0)}},
        {"sphere", {point("center", "중심"), F("radius", "number", "반지름").req().gt(0).unit("length").ex(5.0)}},
        {"cone",
         {point("origin", "밑면 중심"), axis("axis", "축 방향"), F("radius1", "number", "밑면 반지름").req().ge(0).unit("length").ex(5.0),
          F("radius2", "number", "윗면 반지름").req().ge(0).unit("length").ex(2.0),
          F("height", "number", "높이").req().gt(0).unit("length").ex(10.0)}},
        {"torus",
         {point("center", "중심"), axis("axis", "축 방향"), F("major_radius", "number", "큰 반지름").req().gt(0).unit("length").ex(10.0),
          F("minor_radius", "number", "작은 반지름").req().gt(0).unit("length").ex(2.0)}},
        // --- 스윕: 단면(앞 피처까지의 형상의 면 번호, 또는 닫힌 평면 다각형의 점)을 밀거나 돌린다
        {"extrude",
         {profile_face(), profile_points(), profile_sketch(), profile_index(), axis("direction", "미는 방향(없으면 단면의 법선)"),
          F("distance", "number", "미는 거리").req().gt(0).unit("length").ex(10.0), merge()}},
        {"revolve",
         {profile_face(), profile_points(), profile_sketch(), profile_index(), point("point", "회전축 위의 점"), axis("axis", "회전축 방향").req(),
          F("angle", "number", "회전각(도, 기본 360)").gt(0).le(360).ex(90.0), merge()}},
        {"sweep",  // 단면을 경로를 따라 민다: 꺾은선(path) 또는 앞 피처의 모서리(path_edges — 호·스플라인 경로, 파이프 엘보)
         {profile_face(), profile_points(), profile_sketch(), profile_index(),
          F("path", "table", "경로의 꼭짓점(순서대로, 단면 위의 점에서 시작). path_edges 와 둘 중 하나").columns(3).unit("length")
              .ex(rows({{0.0, 0.0, 0.0}, {0.0, 0.0, 50.0}, {30.0, 0.0, 80.0}})),
          F("path_edges", "integer_list", "경로가 되는 모서리 번호(앞 피처까지의 형상 기준, 이어져 있어야 함). 단면은 경로 시작점에 있어야 한다").ge(1).ex(ints({1})),
          merge()}},
        {"loft",  // 단면 여럿을 잇는다: 다각형(points), 점을 지나는 닫힌 스플라인(points + spline), 스케치의 닫힌 영역(sketch + profile)
         {F("profiles", "object_list", "단면(2개 이상). 다각형은 꼭짓점 수가 같아야 매끈하다").req()
              .of({F("points", "table", "닫힌 평면 단면의 점(다각형의 꼭짓점 또는 스플라인이 지날 점)").columns(3).unit("length")
                       .ex(rows({{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {10.0, 10.0, 0.0}, {0.0, 10.0, 0.0}})),
                   F("spline", "bool", "points 를 지나는 닫힌 B-스플라인으로(날개·자유곡면 단면)"),
                   F("sketch", "ref", "단면이 되는 스케치").ref("sketch"), F("profile", "integer", "스케치의 닫힌 영역 번호(기본 1)").ge(1)}),
          F("ruled", "bool", "직선으로 잇는다(기본은 매끈하게)"), merge()}},
        {"surface_grid",  // 점 격자를 지나는 NURBS 곡면(면 바디). 곡면 메싱·자유곡면 시험용
         {F("points", "table", "격자 점(행 우선: 첫 행 columns 개, 다음 행 …). 행·열 모두 2개 이상").columns(3).req().unit("length")
              .ex(rows({{0.0, 0.0, 0.0}, {10.0, 0.0, 2.0}, {0.0, 10.0, 1.0}, {10.0, 10.0, 4.0}})),
          F("columns", "integer", "한 행의 점 수").req().ge(2).ex(2)}},
        // --- 오프셋·두께 부여·솔리드 구성(GEO-18, GEO-29)
        {"offset", {F("distance", "number", "오프셋 거리(바깥쪽이 양)").req().ex(1.0)}},
        {"thicken", {F("thickness", "number", "면(껍질)에 줄 두께. 법선 방향이 양").req().ex(1.0)}},
        {"make_solid", {F("tolerance", "number", "면을 꿰맬 때의 공차").gt(0).ex(1e-3)}},
        {"explode", {}},
        // --- 불리언: 이 파트의 바디들과 tools 파트의 형상
        {"fuse", {tools(false), fuzzy()}},
        {"cut", {tools(true), fuzzy()}},
        {"common", {tools(true), fuzzy()}},
        // --- 분할: 평면으로 바디를 나눈다(나뉜 조각은 같은 파트의 솔리드가 된다)
        {"split", {point("point", "평면 위의 점"), axis("normal", "평면의 법선(datum 이 없으면 필수)"), datum("기준 평면(점·법선 대신)")}},
        // --- 필렛·챔퍼·속 비우기: 앞 피처까지의 형상의 모서리·면 번호로 가리킨다
        {"fillet", {entities("edges", "둥글릴 모서리 번호"), F("radius", "number", "반지름").req().gt(0).unit("length").ex(2.0)}},
        {"chamfer", {entities("edges", "깎을 모서리 번호"), F("distance", "number", "깎는 거리").req().gt(0).unit("length").ex(1.0)}},
        {"shell",
         {entities("faces", "열어 둘 면 번호"), F("thickness", "number", "남길 두께").req().gt(0).unit("length").ex(1.0),
          F("outward", "bool", "바깥쪽으로 두께를 둔다(기본은 안쪽)")}},
        // --- 변환: 파트 전체
        {"translate", {F("vector", "vector3", "이동량").req().unit("length").ex({10.0, 0.0, 0.0})}},
        {"rotate", {point("point", "회전축 위의 점"), axis("axis", "회전축 방향(datum 이 없으면 필수)"), F("angle", "number", "회전각(도)").req().ex(90.0),
                    datum("기준 축(점·방향 대신)")}},
        {"mirror", {point("point", "대칭면 위의 점"), axis("normal", "대칭면의 법선(datum 이 없으면 필수)"), datum("기준 평면(점·법선 대신)")}},
        {"scale", {point("center", "기준점"), F("factor", "number", "배율").req().gt(0).ex(2.0)}},
        {"pattern",
         {F("kind", "string", "배열 종류").req().one_of({"linear", "circular"}).ex("linear"),
          F("count", "integer", "전체 개수(원본 포함)").req().ge(2).ex(3),
          F("vector", "vector3", "linear: 복사 간격").unit("length").ex({10.0, 0.0, 0.0}),
          point("point", "circular: 회전축 위의 점"), axis("axis", "circular: 회전축 방향"),
          F("angle", "number", "circular: 전체 각도(도, 기본 360)").gt(0).le(360).ex(360.0), merge()}},
        // --- 치유(GEO-10): 공차 조정·면 꿰매기·작은 모서리 제거
        {"heal",
         {F("tolerance", "number", "공차(없으면 형상의 것)").gt(0).ex(1e-3), F("sew", "bool", "떨어진 면을 꿰맨다"),
          F("min_edge", "number", "이 길이보다 짧은 모서리를 없앤다").gt(0).unit("length")}},
        // --- 기본 곡선·면(GEO-11): 꼭짓점·모서리·면 바디를 더한다
        {"point", {point("point", "점").req()}},
        {"line", {point("start", "시작점").req(), point("end", "끝점").req()}},
        {"arc", {point("start", "시작점").req(), point("middle", "호 위의 가운데 점").req(), point("end", "끝점").req()}},
        {"spline", {F("points", "table", "지나는 점(3개 이상)").columns(3).req().unit("length").ex(rows({{0.0, 0.0, 0.0}, {5.0, 3.0, 0.0}, {10.0, 0.0, 0.0}}))}},
        {"plane", {point("point", "중심"), axis("normal", "법선"), F("size", "number", "한 변의 길이").req().gt(0).unit("length").ex(10.0)}},
        // --- 임프린트·공유 토폴로지(GEO-14)
        {"imprint", {tools(true)}},  // 도구 파트의 면·모서리가 지나는 자리에 이 파트의 면을 나눈다(부피는 그대로)
        {"share_topology", {tools(false)}},  // 맞닿은 바디(와 도구 파트)들이 경계 면을 공유하게 다시 만든다(절점 일치 메싱용)
        // --- 단순화(GEO-16): 면을 없애고 이웃 면을 늘여 메운다(필렛·구멍·돌기)
        {"remove_fillet", {entities("faces", "없앨 필렛 면 번호")}},
        {"remove_hole", {entities("faces", "없앨 구멍의 면 번호(원통면 등)")}},
        {"remove_faces", {entities("faces", "없앨 면 번호")}},
        {"merge_faces", {F("angle", "number", "같은 곡면으로 볼 각도 허용(도, 기본 0.1)").ge(0)}},
        {"merge_edges", {F("angle", "number", "같은 곡선으로 볼 각도 허용(도, 기본 0.1)").ge(0)}},
        // --- 중립면(GEO-17): 마주 보는 두 면의 가운데 면 하나(평면·동심 원통처럼 오프셋이 되는 쌍)
        {"midsurface", {entities("faces", "마주 보는 두 면 번호 [a, b]"), F("keep", "bool", "원래 솔리드를 남긴다(기본 지움)")}},
        // --- 곡선 편집(GEO-21~25): 모서리 번호(앞 피처까지의 형상 기준). 결과는 새 모서리 바디로 더한다
        {"curve_trim", {F("edge", "integer", "모서리 번호").req().ge(1).ex(1), F("start", "number", "남길 구간의 시작(0~1)").ge(0).le(1).ex(0.0),
                        F("end", "number", "남길 구간의 끝(0~1)").ge(0).le(1).ex(0.5)}},
        {"curve_extend", {F("edge", "integer", "모서리 번호").req().ge(1).ex(1), F("length", "number", "늘릴 길이").req().gt(0).unit("length").ex(5.0),
                          F("at", "string", "늘릴 쪽").one_of({"start", "end", "both"})}},
        {"curve_split", {F("edge", "integer", "모서리 번호").req().ge(1).ex(1), F("at", "number", "나눌 자리(0~1)").req().gt(0).lt(1).ex(0.5)}},
        {"curve_join", {entities("edges", "이을 모서리 번호(이어져 있어야 함)")}},
        {"curve_project", {F("edge", "integer", "투영할 모서리 번호").req().ge(1).ex(1), F("face", "integer", "받을 면 번호").req().ge(1).ex(1)}},
        {"curve_intersect", {F("face_a", "integer", "면 번호").req().ge(1).ex(1), F("face_b", "integer", "면 번호").req().ge(1).ex(2)}},
        {"curve_offset", {F("edge", "integer", "평면 곡선의 모서리 번호").req().ge(1).ex(1), F("distance", "number", "오프셋 거리").req().unit("length").ex(2.0),
                          axis("normal", "곡선이 놓인 평면의 법선")}},
        // --- 곡면 편집(GEO-26~28)
        {"face_fill", {entities("edges", "닫힌 고리를 이루는 모서리 번호")}},
        {"face_extend", {F("face", "integer", "평면 면 번호").req().ge(1).ex(1), F("length", "number", "둘레를 바깥으로 늘릴 길이").req().gt(0).unit("length").ex(2.0)}},
        {"face_trim", {F("face", "integer", "자를 면 번호").req().ge(1).ex(1), point("point", "자르는 평면 위의 점"), axis("normal", "자르는 평면의 법선(남길 쪽)").req()}},
        {"face_delete", {entities("faces", "지울 면 번호")}},
        {"face_replace", {F("face", "integer", "바꿀 면 번호").req().ge(1).ex(1), F("with", "integer", "대신 쓸 면 번호").req().ge(1).ex(2),
                          F("tolerance", "number", "꿰맬 공차").gt(0).ex(1e-3)}},
        // --- 형상 피처(GEO-31)
        {"hole", {point("point", "구멍 중심(면 위의 점)").req(), axis("direction", "뚫는 방향"), F("diameter", "number", "지름").req().gt(0).unit("length").ex(5.0),
                  F("depth", "number", "깊이(없으면 관통)").gt(0).unit("length")}},
        {"pocket", {profile_face(), profile_points(), profile_sketch(), profile_index(), axis("direction", "파는 방향(없으면 단면 법선의 반대)"),
                    F("depth", "number", "깊이").req().gt(0).unit("length").ex(5.0)}},
        {"boss", {profile_face(), profile_points(), profile_sketch(), profile_index(), axis("direction", "올리는 방향(없으면 단면의 법선)"),
                  F("height", "number", "높이").req().gt(0).unit("length").ex(5.0)}},
        {"rib", {F("path", "table", "리브의 중심선 꼭짓점(순서대로)").columns(3).req().unit("length").ex(rows({{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}})),
                 F("thickness", "number", "두께").req().gt(0).unit("length").ex(2.0), F("height", "number", "높이").req().gt(0).unit("length").ex(5.0),
                 axis("direction", "높이 방향").req(), axis("normal", "두께 방향(없으면 경로와 높이 방향에 수직)")}},
        {"draft", {entities("faces", "기울일 면 번호"), F("angle", "number", "기울기(도)").req().ex(5.0), axis("direction", "뽑는 방향").req(),
                   point("point", "중립 평면 위의 점"), axis("normal", "중립 평면의 법선(없으면 뽑는 방향)")}},
    };
    s.add(std::move(k));
  }
}

}  // namespace nasa95::kinds
