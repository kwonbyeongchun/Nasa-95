// 형상(5단계): 피처에서 파트의 형상을 계산하고, 가져오기·내보내기·조회·측정·검사·표시용 삼각화를 한다.
// OpenCASCADE 는 이 파일 안에서만 쓴다(아키텍처 규칙 2).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <optional>
#include <memory>
#include <set>
#include <sstream>

#include "geometry_occt.hpp"
#include "sketch_solver.hpp"
#include "ofep/error.hpp"
#include "ofep/geometry.hpp"

#ifdef OFEP_WITH_OCCT
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAlgo_NormalProjection.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_BSplineCurve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <BRepFill_Filling.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepTools_ReShape.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomConvert.hxx>
#include <GeomConvert_CompCurveToBSplineCurve.hxx>
#include <GeomLib.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Line.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Wireframe.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>
#include <gp_Torus.hxx>
#include <gp_Cone.hxx>
#include <gp_Sphere.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Elips.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <IGESControl_Reader.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <Quantity_Color.hxx>
#include <TCollection_ExtendedString.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <NCollection_DataMap.hxx>
#include <TopExp_Explorer.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>
#endif

namespace ofep {

namespace {

using F = FieldSpec;

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }
std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }

CommandSpec base(const std::string& name, char kind, const std::string& target, const std::string& desc, const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = (kind == 'C'), c.target = target, c.desc = desc, c.features = features;
  return c;
}

const Object& part_of(const App& a, const Json& p) {
  const Object& o = a.model().get(p["id"].get<Id>());
  if (o.kind != "part") throw Error("wrong_kind", "id=" + std::to_string(o.id) + " 는 파트가 아닙니다", {{"object", o.id}, {"expected", "part"}});
  return o;
}

}  // namespace

#ifdef OFEP_WITH_OCCT

bool geometry_available() { return true; }

namespace {

const double kPi = 3.14159265358979323846;

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using ShapeList = NCollection_List<TopoDS_Shape>;
using AncestorMap = NCollection_IndexedDataMap<TopoDS_Shape, ShapeList, TopTools_ShapeMapHasher>;

struct PartShape {
  std::string digest;
  TopoDS_Shape shape;
  Json status = Json::array();
  ShapeMap solids, faces, edges, vertices;
  // 영속 이름표(D9): 종류별 번호-1 → 이름, 이름 → 번호
  std::map<std::string, std::vector<std::string>> names;
  std::map<std::string, std::map<std::string, int>> index_of_name;
  bool ok = true;
};
struct GeoStore {
  std::map<Id, PartShape> parts;
};
GeoStore& store(App& a) {
  std::any& slot = a.runtime("geometry");
  if (!slot.has_value()) slot = std::make_shared<GeoStore>();
  return *std::any_cast<std::shared_ptr<GeoStore>&>(slot);
}

gp_Pnt pnt(const Json& p, const char* key) {
  if (!has(p, key)) return gp_Pnt(0, 0, 0);
  return gp_Pnt(p[key][0].get<double>(), p[key][1].get<double>(), p[key][2].get<double>());
}
gp_Dir dir(const Json& p, const char* key, bool required = false) {
  if (!has(p, key)) {
    if (required) throw Error("missing_param", std::string("필수 속성이 없습니다: ") + key, {{"param", key}});
    return gp_Dir(0, 0, 1);
  }
  const double x = p[key][0].get<double>(), y = p[key][1].get<double>(), z = p[key][2].get<double>();
  if (x * x + y * y + z * z == 0.0) throw Error("out_of_range", std::string("'") + key + "' 의 길이가 0 입니다", {{"param", key}});
  return gp_Dir(x, y, z);
}
double number(const Json& p, const char* key) {
  if (!has(p, key)) throw Error("missing_param", std::string("필수 속성이 없습니다: ") + key, {{"param", key}});
  return p[key].get<double>();
}

TopoDS_Shape compound_of(const std::vector<TopoDS_Shape>& bodies) {
  if (bodies.size() == 1) return bodies[0];
  TopoDS_Compound c;
  BRep_Builder b;
  b.MakeCompound(c);
  for (const TopoDS_Shape& s : bodies) b.Add(c, s);
  return c;
}

TopoDS_Shape read_brep(const std::string& text) {
  // 형식이 아닌 글을 넘기면 형상 커널이 표준 출력에 경고를 쓰므로 먼저 걸러 낸다
  if (text.find("CASCADE Topology") == std::string::npos && text.find("DBRep_DrawableShape") == std::string::npos)
    throw Error("invalid_geometry", "BREP 형상이 아닙니다", {{"param", "brep"}});
  TopoDS_Shape s;
  BRep_Builder b;
  std::istringstream is(text);
  BRepTools::Read(s, is, b);
  if (s.IsNull()) throw Error("invalid_geometry", "BREP 형상을 읽을 수 없습니다", {{"param", "brep"}});
  return s;
}
std::string write_brep(const TopoDS_Shape& s) {
  std::ostringstream os;
  BRepTools::Write(s, os);
  return os.str();
}

// 파트의 형상이 무엇에 달려 있는지 요약한 글. 이것이 바뀌면 다시 계산한다.
std::string digest_of(const App& a, Id part, std::set<Id>& stack) {
  if (!stack.insert(part).second) throw Error("invalid_state", "파트가 서로를 불리언 도구로 가리킵니다(순환)", {{"object", part}});
  const Object& p = a.model().get(part);
  std::string d = "part" + std::to_string(part) + "|" + p.props.value("rollback", Json()).dump();
  for (const Object* f : a.model().children(part, "feature")) {
    // 가져온 형상의 글은 길다: 길이와 앞·뒤 일부만 본다(내용이 바뀌면 객체도 바뀐다)
    Json props = f->props;
    if (props.contains("brep") && props["brep"].is_string()) {
      const std::string& b = props["brep"].get_ref<const std::string&>();
      props["brep"] = std::to_string(b.size()) + ":" + std::to_string(std::hash<std::string>{}(b));
    }
    d += "|" + std::to_string(f->id) + (f->suppressed ? "s" : "") + props.dump();
    if (has(f->props, "datum"))  // 기준 형상이 바뀌면 다시 계산한다
      if (const Object* dt = a.model().find(f->props["datum"].get<Id>())) d += "{datum" + dt->props.dump() + "}";
    if (has(f->props, "sketch"))  // 스케치가 바뀌면 다시 계산한다
      if (const Object* sk = a.model().find(f->props["sketch"].get<Id>())) d += "{sketch" + sk->props.dump() + "}";
    if (!f->suppressed && has(f->props, "tools"))
      for (const Json& t : f->props["tools"]) d += "{" + digest_of(a, t.get<Id>(), stack) + "}";
  }
  stack.erase(part);
  return d;
}

const PartShape& shape_of(App& a, Id part);

TopoDS_Shape transformed(const TopoDS_Shape& s, const gp_Trsf& t) { return BRepBuilderAPI_Transform(s, t, true).Shape(); }

// 지금까지의 바디들의 엔티티 번호표(geometry.entities 와 같은 순서: TopExp::MapShapes).
ShapeMap current_map(const std::vector<TopoDS_Shape>& bodies, TopAbs_ShapeEnum kind) {
  ShapeMap m;
  if (!bodies.empty()) TopExp::MapShapes(compound_of(bodies), kind, m);
  return m;
}
const TopoDS_Shape& entity_of(const ShapeMap& m, int index, const char* what) {
  if (index < 1 || index > m.Extent())
    throw Error("not_found", std::string("없는 엔티티입니다: ") + what + " " + std::to_string(index), {{"param", what}, {"count", m.Extent()}});
  return m(index);
}

// 평면 면의 법선(면의 방향을 반영).
gp_Dir face_normal(const TopoDS_Face& face) {
  BRepAdaptor_Surface surf(face);
  if (surf.GetType() != GeomAbs_Plane) throw Error("invalid_geometry", "평면이 아닌 면은 direction 을 적어야 합니다", {{"param", "direction"}});
  gp_Dir n = surf.Plane().Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
  return n;
}

// ------------------------------------------------------------ 스케치(GEO-32~38)
struct SketchFrame {
  gp_Pnt origin;
  gp_Dir normal, u, v;
};
SketchFrame sketch_frame(App& a, const Object& sk) {
  Json q = sk.props;
  if (has(q, "datum")) {
    const Object* d = a.model().find(q["datum"].get<Id>());
    if (!d || d->kind != "datum" || subtype_of(*d) != "plane") throw Error("not_found", "기준 평면이 없습니다", {{"param", "datum"}});
    if (has(d->props, "point")) q["point"] = d->props["point"];
    q["normal"] = d->props["normal"];
  }
  SketchFrame f{pnt(q, "point"), dir(q, "normal"), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)};
  gp_Ax3 ax(f.origin, f.normal);
  if (has(q, "x_axis")) {
    const gp_Dir x = dir(q, "x_axis");
    if (std::fabs(x.Dot(f.normal)) > 1.0 - 1e-9) throw Error("invalid_param", "x_axis 가 법선과 나란합니다", {{"param", "x_axis"}});
    ax = gp_Ax3(f.origin, f.normal, x);
  }
  f.u = ax.XDirection(), f.v = ax.YDirection();
  return f;
}
gp_Pnt sketch_pt(const SketchFrame& f, const Json& uv) {
  if (!uv.is_array() || uv.size() < 2) throw Error("invalid_param", "스케치 점은 [u, v] 입니다", {{"value", uv}});
  return f.origin.Translated(gp_Vec(f.u) * uv[0].get<double>() + gp_Vec(f.v) * uv[1].get<double>());
}
// 요소 하나 → 3D 모서리들(점은 꼭짓점)
std::vector<TopoDS_Shape> sketch_edges(const SketchFrame& f, const Json& e) {
  const std::string kind = e.value("kind", std::string());
  std::vector<TopoDS_Shape> out;
  if (kind == "line") {
    BRepBuilderAPI_MakeEdge mk(sketch_pt(f, e["start"]), sketch_pt(f, e["end"]));
    if (!mk.IsDone()) throw Error("invalid_geometry", "선의 두 점이 같습니다", {{"entity", e.value("id", 0)}});
    out.push_back(mk.Shape());
  } else if (kind == "circle") {
    out.push_back(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(sketch_pt(f, e["center"]), f.normal, f.u), e["radius"].get<double>())).Shape());
  } else if (kind == "arc") {
    GC_MakeArcOfCircle arc(sketch_pt(f, e["start"]), sketch_pt(f, e["middle"]), sketch_pt(f, e["end"]));
    if (!arc.IsDone()) throw Error("invalid_geometry", "호의 세 점이 한 직선 위에 있습니다", {{"entity", e.value("id", 0)}});
    out.push_back(BRepBuilderAPI_MakeEdge(arc.Value()).Shape());
  } else if (kind == "rectangle") {
    const double u0 = e["corner"][0].get<double>(), v0 = e["corner"][1].get<double>(), w = e["size"][0].get<double>(), h = e["size"][1].get<double>();
    const Json c[4] = {Json::array({u0, v0}), Json::array({u0 + w, v0}), Json::array({u0 + w, v0 + h}), Json::array({u0, v0 + h})};
    for (int k = 0; k < 4; ++k) out.push_back(BRepBuilderAPI_MakeEdge(sketch_pt(f, c[k]), sketch_pt(f, c[(k + 1) % 4])).Shape());
  } else if (kind == "ellipse") {
    const double R = e["radii"][0].get<double>(), r = e["radii"][1].get<double>(), ang = e.value("angle", 0.0) * kPi / 180.0;
    if (r > R) throw Error("invalid_geometry", "타원의 작은 반지름이 큰 반지름보다 큽니다", {{"entity", e.value("id", 0)}});
    gp_Dir major(gp_Vec(f.u) * std::cos(ang) + gp_Vec(f.v) * std::sin(ang));
    out.push_back(BRepBuilderAPI_MakeEdge(gp_Elips(gp_Ax2(sketch_pt(f, e["center"]), f.normal, major), R, r)).Shape());
  } else if (kind == "spline") {
    const Json pts = e.value("points", Json::array());
    if (pts.size() < 2) throw Error("invalid_geometry", "스플라인에는 점이 2개 이상 필요합니다", {{"entity", e.value("id", 0)}});
    TColgp_Array1OfPnt arr(1, static_cast<int>(pts.size()));
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) arr.SetValue(i + 1, sketch_pt(f, pts[i]));
    GeomAPI_PointsToBSpline mk(arr);
    if (!mk.IsDone()) throw Error("invalid_geometry", "스플라인을 만들 수 없습니다", {{"entity", e.value("id", 0)}});
    out.push_back(BRepBuilderAPI_MakeEdge(mk.Curve()).Shape());
  } else if (kind == "point") {
    out.push_back(BRepBuilderAPI_MakeVertex(sketch_pt(f, e["position"])).Shape());
  } else {
    throw Error("invalid_param", "모르는 스케치 요소 종류입니다: " + kind, {{"entity", e.value("id", 0)}});
  }
  return out;
}
// 닫힌 영역: 평면의 큰 사각형 면을 모든 모서리로 쪼개고, 큰 사각형의 테두리에 닿지 않는 면이 영역이다(큰 면의 테두리를 넘지 않는 둘레)
std::vector<TopoDS_Face> sketch_profiles(App& a, const Object& sk) {
  const SketchFrame f = sketch_frame(a, sk);
  ShapeList edges;
  double extent = 1.0;
  for (const Json& e : sk.props.value("entities", Json::array())) {
    if (e.value("reference", false) || e.value("kind", std::string()) == "point") continue;
    for (const TopoDS_Shape& s : sketch_edges(f, e)) {
      edges.Append(s);
      Bnd_Box box;
      BRepBndLib::Add(s, box);
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      extent = std::max({extent, std::fabs(x0 - f.origin.X()), std::fabs(x1 - f.origin.X()), std::fabs(y0 - f.origin.Y()), std::fabs(y1 - f.origin.Y()),
                         std::fabs(z0 - f.origin.Z()), std::fabs(z1 - f.origin.Z())});
    }
  }
  std::vector<TopoDS_Face> out;
  if (edges.IsEmpty()) return out;
  const double half = 4.0 * extent;
  const TopoDS_Face big = BRepBuilderAPI_MakeFace(gp_Pln(gp_Ax3(f.origin, f.normal, f.u)), -half, half, -half, half).Face();
  BRepAlgoAPI_Splitter op;
  ShapeList args;
  args.Append(big);
  op.SetArguments(args), op.SetTools(edges), op.Build();
  if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "스케치의 닫힌 영역을 찾지 못했습니다");
  ShapeMap big_edges;
  TopExp::MapShapes(big, TopAbs_EDGE, big_edges);
  // 큰 사각형의 테두리 모서리(쪼개진 것 포함)는 바깥 영역의 것: 결과 면 가운데 테두리 모서리의 Modified/자기 자신을 가진 면을 뺀다
  std::set<const void*> rim;
  for (int i = 1; i <= big_edges.Extent(); ++i) {
    rim.insert(big_edges(i).TShape().get());
    for (const TopoDS_Shape& mod : op.Modified(big_edges(i))) rim.insert(mod.TShape().get());
  }
  for (TopExp_Explorer ex(op.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
    bool outer = false;
    for (TopExp_Explorer ee(ex.Current(), TopAbs_EDGE); ee.More() && !outer; ee.Next())
      if (rim.count(ee.Current().TShape().get())) outer = true;
    if (!outer) out.push_back(TopoDS::Face(ex.Current()));
  }
  // 면 번호를 일정하게: 무게중심의 (u, v) 순
  std::sort(out.begin(), out.end(), [&](const TopoDS_Face& x, const TopoDS_Face& y) {
    GProp_GProps gx, gy;
    BRepGProp::SurfaceProperties(x, gx), BRepGProp::SurfaceProperties(y, gy);
    const gp_Vec dx(f.origin, gx.CentreOfMass()), dy(f.origin, gy.CentreOfMass());
    const double ux = dx.Dot(gp_Vec(f.u)), uy = dy.Dot(gp_Vec(f.u)), vx = dx.Dot(gp_Vec(f.v)), vy = dy.Dot(gp_Vec(f.v));
    return std::fabs(vx - vy) > 1e-9 ? vx < vy : ux < uy;
  });
  return out;
}

// 스윕의 단면: 앞 형상의 면(face), 닫힌 평면 다각형(points) 또는 스케치의 닫힌 영역(sketch·profile).
TopoDS_Face profile_face(App& a, const Json& q, const std::vector<TopoDS_Shape>& bodies);
TopoDS_Face profile_face(const Json& q, const std::vector<TopoDS_Shape>& bodies) {
  if (has(q, "face")) {
    if (bodies.empty()) throw Error("invalid_state", "이 피처보다 앞에 형상이 있어야 합니다");
    return TopoDS::Face(entity_of(current_map(bodies, TopAbs_FACE), q["face"].get<int>(), "face"));
  }
  if (!has(q, "points")) throw Error("missing_param", "단면이 없습니다: face 또는 points", {{"param", "points"}});
  const Json& pts = q["points"];
  if (pts.size() < 3) throw Error("out_of_range", "다각형에는 점이 3개 이상 있어야 합니다", {{"param", "points"}});
  BRepBuilderAPI_MakePolygon poly;
  for (const Json& p : pts) poly.Add(gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
  poly.Close();
  if (!poly.IsDone()) throw Error("invalid_geometry", "다각형을 만들 수 없습니다(점이 겹침)", {{"param", "points"}});
  BRepBuilderAPI_MakeFace mk(poly.Wire(), true);
  if (!mk.IsDone()) throw Error("invalid_geometry", "다각형이 한 평면 위에 있지 않거나 자기 교차합니다", {{"param", "points"}});
  return mk.Face();
}
TopoDS_Face profile_face(App& a, const Json& q, const std::vector<TopoDS_Shape>& bodies) {
  if (has(q, "sketch")) {
    const Object* sk = a.model().find(q["sketch"].get<Id>());
    if (!sk || sk->kind != "sketch") throw Error("not_found", "스케치가 없습니다", {{"param", "sketch"}});
    const std::vector<TopoDS_Face> faces = sketch_profiles(a, *sk);
    const int index = q.value("profile", 1);
    if (faces.empty()) throw Error("invalid_geometry", "스케치에 닫힌 영역이 없습니다", {{"param", "sketch"}, {"object", sk->id}});
    if (index < 1 || index > static_cast<int>(faces.size()))
      throw Error("out_of_range", "스케치의 닫힌 영역 번호가 범위를 벗어났습니다", {{"param", "profile"}, {"count", faces.size()}});
    return faces[static_cast<std::size_t>(index - 1)];
  }
  return profile_face(q, bodies);
}

// 단면으로 쓴 면이 독립 면 바디(face_fill·plane 등)였으면 바디 목록에서 뺀다 — 돌출·회전·스윕 결과에 흡수된다.
// 남겨 두면 같은 면이 두 번 있어 모서리가 비다양체로 잡힌다. 솔리드의 면을 단면으로 썼을 때는 아무것도 바뀌지 않는다
void consume_profile_body(std::vector<TopoDS_Shape>& bodies, const TopoDS_Face& profile) {
  std::vector<TopoDS_Shape> kept;
  for (const TopoDS_Shape& b : bodies)
    if (!(b.ShapeType() == TopAbs_FACE && b.IsSame(profile))) kept.push_back(b);
  bodies = kept;
}

// 새로 만든 바디를 앞 형상에 합친다(merge: none·fuse·cut).
void merge_body(const Json& q, std::vector<TopoDS_Shape>& bodies, const std::vector<TopoDS_Shape>& made) {
  const std::string merge = q.value("merge", std::string("none"));
  if (merge == "none" || bodies.empty()) {
    if (merge == "cut" && bodies.empty()) throw Error("invalid_state", "뺄 형상이 앞에 없습니다");
    bodies.insert(bodies.end(), made.begin(), made.end());
    return;
  }
  ShapeList args, tools;
  for (const TopoDS_Shape& b : bodies) args.Append(b);
  for (const TopoDS_Shape& m : made) tools.Append(m);
  TopoDS_Shape result;
  if (merge == "fuse") {
    BRepAlgoAPI_Fuse op;
    op.SetArguments(args), op.SetTools(tools), op.Build();
    if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "합치기에 실패했습니다");
    result = op.Shape();
  } else {
    BRepAlgoAPI_Cut op;
    op.SetArguments(args), op.SetTools(tools), op.Build();
    if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "빼기에 실패했습니다");
    result = op.Shape();
  }
  ShapeUpgrade_UnifySameDomain unify(result, true, true, false);
  unify.Build();
  if (!unify.Shape().IsNull()) result = unify.Shape();
  if (!TopExp_Explorer(result, TopAbs_FACE).More()) throw Error("geometry_failed", "결과가 비었습니다");
  bodies.assign(1, result);
}

// 피처의 점·방향: datum(기준 형상)이 있으면 그것의 점·방향을 쓴다(GEO-19).
Json with_datum(App& a, const Object& f, const char* dir_key) {
  Json q = f.props;
  if (!has(q, "datum")) return q;
  const Object* d = a.model().find(q["datum"].get<Id>());
  if (!d || d->kind != "datum") throw Error("not_found", "기준 형상이 없습니다", {{"param", "datum"}});
  const std::string dt = subtype_of(*d);
  if (has(d->props, "point")) q["point"] = d->props["point"];
  if (dt == "axis" && has(d->props, "direction")) q[dir_key] = d->props["direction"];
  else if (dt == "plane" && has(d->props, "normal")) q[dir_key] = d->props["normal"];
  else throw Error("invalid_param", "기준 형상에 방향이 없습니다(점이 아니라 축·평면이어야 한다)", {{"param", "datum"}});
  return q;
}

// 피처 하나를 적용한다.
void apply(App& a, const Object& f, std::vector<TopoDS_Shape>& bodies) {
  const std::string t = subtype_of(f);
  const Json q = (t == "split" || t == "mirror") ? with_datum(a, f, "normal") : t == "rotate" ? with_datum(a, f, "axis") : f.props;
  auto need_body = [&] {
    if (bodies.empty()) throw Error("invalid_state", "이 피처보다 앞에 형상이 있어야 합니다");
  };
  auto transform_all = [&](const gp_Trsf& trsf) {
    need_body();
    // 바디를 하나씩 복사·변환하면 공유 토폴로지(share_topology 로 함께 쓰는 경계 면)가 바디마다 따로 복사돼 풀린다.
    // 컴파운드 전체를 한 번에 변환하고 바디를 다시 꺼내 공유를 유지한다
    BRepBuilderAPI_Transform op(compound_of(bodies), trsf, true);
    std::vector<TopoDS_Shape> moved;
    for (const TopoDS_Shape& b : bodies) moved.push_back(op.ModifiedShape(b));
    bodies = moved;
  };
  if (t == "import") {
    TopoDS_Shape s = read_brep(q.value("brep", std::string()));
    if (has(q, "scale") && q["scale"].get<double>() != 1.0) {
      gp_Trsf trsf;
      trsf.SetScale(gp_Pnt(0, 0, 0), q["scale"].get<double>());
      s = transformed(s, trsf);
    }
    bodies.push_back(s);
  } else if (t == "box") {
    if (!has(q, "size")) throw Error("missing_param", "필수 속성이 없습니다: size", {{"param", "size"}});
    const double dx = q["size"][0].get<double>(), dy = q["size"][1].get<double>(), dz = q["size"][2].get<double>();
    if (dx <= 0 || dy <= 0 || dz <= 0) throw Error("out_of_range", "크기는 0 보다 커야 합니다", {{"param", "size"}});
    bodies.push_back(BRepPrimAPI_MakeBox(pnt(q, "origin"), dx, dy, dz).Shape());
  } else if (t == "cylinder") {
    bodies.push_back(BRepPrimAPI_MakeCylinder(gp_Ax2(pnt(q, "origin"), dir(q, "axis")), number(q, "radius"), number(q, "height")).Shape());
  } else if (t == "sphere") {
    bodies.push_back(BRepPrimAPI_MakeSphere(pnt(q, "center"), number(q, "radius")).Shape());
  } else if (t == "cone") {
    const double r1 = number(q, "radius1"), r2 = number(q, "radius2");
    if (r1 == r2) throw Error("out_of_range", "두 반지름이 같으면 원뿔이 아닙니다(실린더를 쓴다)", {{"param", "radius2"}});
    bodies.push_back(BRepPrimAPI_MakeCone(gp_Ax2(pnt(q, "origin"), dir(q, "axis")), r1, r2, number(q, "height")).Shape());
  } else if (t == "torus") {
    const double R = number(q, "major_radius"), r = number(q, "minor_radius");
    if (r >= R) throw Error("out_of_range", "작은 반지름은 큰 반지름보다 작아야 합니다", {{"param", "minor_radius"}});
    bodies.push_back(BRepPrimAPI_MakeTorus(gp_Ax2(pnt(q, "center"), dir(q, "axis")), R, r).Shape());
  } else if (t == "fuse" || t == "cut" || t == "common") {
    need_body();
    ShapeList args, tools;
    for (const TopoDS_Shape& b : bodies) args.Append(b);
    for (const Json& id : q.value("tools", Json::array())) {
      const PartShape& other = shape_of(a, id.get<Id>());
      if (other.shape.IsNull()) throw Error("invalid_state", "도구 파트에 형상이 없습니다", {{"object", id}});
      tools.Append(other.shape);
    }
    if (t == "fuse") {  // 이 파트의 바디들과 도구를 모두 합친다
      if (tools.IsEmpty()) {
        if (bodies.size() < 2) return;  // 합칠 것이 없다
        args.Clear();
        args.Append(bodies[0]);
        for (std::size_t i = 1; i < bodies.size(); ++i) tools.Append(bodies[i]);
      }
    } else if (tools.IsEmpty()) {
      throw Error("missing_param", "필수 속성이 없습니다: tools", {{"param", "tools"}});
    }
    TopoDS_Shape result;
    auto run = [&](BRepAlgoAPI_BooleanOperation& op) {
      op.SetArguments(args);
      op.SetTools(tools);
      // 퍼지 공차: 거의 붙은 면·작은 틈·겹침을 같은 것으로 본다(가져온 형상의 조립 틈을 메우며 합칠 때)
      if (has(q, "tolerance")) op.SetFuzzyValue(q["tolerance"].get<double>());
      op.Build();
      if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "불리언 연산에 실패했습니다");
      result = op.Shape();
    };
    if (t == "fuse") {
      BRepAlgoAPI_Fuse op;
      run(op);
    } else if (t == "cut") {
      BRepAlgoAPI_Cut op;
      run(op);
    } else {
      BRepAlgoAPI_Common op;
      run(op);
    }
    // 같은 곡면 위에 나뉜 면·모서리를 합친다(불리언 뒤에 남는 조각)
    ShapeUpgrade_UnifySameDomain unify(result, true, true, false);
    unify.Build();
    if (!unify.Shape().IsNull()) result = unify.Shape();
    if (TopExp_Explorer(result, TopAbs_FACE).More() == false) throw Error("geometry_failed", "불리언 결과가 비었습니다");
    bodies.assign(1, result);
  } else if (t == "extrude" || t == "revolve") {
    const TopoDS_Face profile = profile_face(a, q, bodies);
    TopoDS_Shape made;
    if (t == "extrude") {
      gp_Dir d = has(q, "direction") ? dir(q, "direction") : face_normal(profile);
      made = BRepPrimAPI_MakePrism(profile, gp_Vec(d) * number(q, "distance")).Shape();
    } else {
      const double angle = q.value("angle", 360.0) * kPi / 180.0;
      made = BRepPrimAPI_MakeRevol(profile, gp_Ax1(pnt(q, "point"), dir(q, "axis", true)), angle).Shape();
    }
    if (made.IsNull() || !TopExp_Explorer(made, TopAbs_FACE).More()) throw Error("geometry_failed", "스윕 결과가 비었습니다");
    consume_profile_body(bodies, profile);
    merge_body(q, bodies, {made});
  } else if (t == "sweep") {
    TopoDS_Wire path;
    if (has(q, "path_edges")) {  // 앞 피처의 모서리(호·스플라인 포함)를 이어 경로로 — 파이프 엘보 등 곡선 경로
      if (bodies.empty()) throw Error("invalid_state", "이 피처보다 앞에 경로 모서리가 있어야 합니다", {{"param", "path_edges"}});
      BRepBuilderAPI_MakeWire wire;
      const ShapeMap edges = current_map(bodies, TopAbs_EDGE);
      for (const Json& e : q["path_edges"]) wire.Add(TopoDS::Edge(entity_of(edges, e.get<int>(), "edge")));
      if (!wire.IsDone()) throw Error("invalid_geometry", "경로 모서리가 이어져 있지 않습니다", {{"param", "path_edges"}});
      path = wire.Wire();
      // 경로로 쓴 모서리 바디(선·호 피처)는 결과에 남기지 않는다(경로는 보조 형상)
      std::vector<TopoDS_Shape> kept;
      for (const TopoDS_Shape& b : bodies) {
        bool is_path_edge = false;
        if (b.ShapeType() == TopAbs_EDGE)
          for (const Json& e : q["path_edges"]) is_path_edge = is_path_edge || b.IsSame(entity_of(edges, e.get<int>(), "edge"));
        if (!is_path_edge) kept.push_back(b);
      }
      bodies = kept;
    } else {
      if (!has(q, "path") || q["path"].size() < 2) throw Error("missing_param", "경로가 없습니다: path(점 2개 이상) 또는 path_edges", {{"param", "path"}});
      BRepBuilderAPI_MakePolygon poly;
      for (const Json& p : q["path"]) poly.Add(gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
      if (!poly.IsDone()) throw Error("invalid_geometry", "경로를 만들 수 없습니다(점이 겹침)", {{"param", "path"}});
      path = poly.Wire();
    }
    const TopoDS_Face profile = profile_face(a, q, bodies);
    // 꺾은선의 모퉁이는 맞꺾기(RightCorner)로 잇는다(단순 파이프는 직각 모퉁이에서 자기 교차한다). 곡선 경로에서는 영향이 없다
    BRepOffsetAPI_MakePipeShell pipe(path);
    pipe.SetTransitionMode(BRepBuilderAPI_RightCorner);
    pipe.Add(BRepTools::OuterWire(profile));
    pipe.Build();
    if (!pipe.IsDone() || !pipe.MakeSolid() || pipe.Shape().IsNull()) throw Error("geometry_failed", "스윕에 실패했습니다");
    consume_profile_body(bodies, profile);
    merge_body(q, bodies, {pipe.Shape()});
  } else if (t == "loft") {
    const Json profiles = q.value("profiles", Json::array());
    if (profiles.size() < 2) throw Error("out_of_range", "단면이 2개 이상 있어야 합니다", {{"param", "profiles"}});
    BRepOffsetAPI_ThruSections loft(true, q.value("ruled", false));
    for (const Json& pr : profiles) {
      if (has(pr, "sketch")) {  // 스케치의 닫힌 영역을 단면으로
        Json sel{{"sketch", pr["sketch"]}, {"profile", pr.value("profile", 1)}};
        loft.AddWire(BRepTools::OuterWire(profile_face(a, sel, bodies)));
        continue;
      }
      const Json pts = pr.value("points", Json::array());
      if (pts.size() < 3) throw Error("invalid_geometry", "단면에는 점이 3개 이상 있어야 합니다(또는 sketch)", {{"param", "profiles"}});
      if (pr.value("spline", false)) {  // 점을 지나는 닫힌(주기) B-스플라인
        Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, static_cast<int>(pts.size()));
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) arr->SetValue(i + 1, gp_Pnt(pts[i][0].get<double>(), pts[i][1].get<double>(), pts[i][2].get<double>()));
        GeomAPI_Interpolate interp(arr, true, 1e-7);
        interp.Perform();
        if (!interp.IsDone()) throw Error("invalid_geometry", "단면 스플라인을 만들 수 없습니다", {{"param", "profiles"}});
        BRepBuilderAPI_MakeWire wire(BRepBuilderAPI_MakeEdge(interp.Curve()).Edge());
        loft.AddWire(wire.Wire());
        continue;
      }
      BRepBuilderAPI_MakePolygon poly;
      for (const Json& p : pts) poly.Add(gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
      poly.Close();
      if (!poly.IsDone()) throw Error("invalid_geometry", "단면 다각형을 만들 수 없습니다", {{"param", "profiles"}});
      loft.AddWire(poly.Wire());
    }
    loft.Build();
    if (!loft.IsDone() || loft.Shape().IsNull()) throw Error("geometry_failed", "로프트에 실패했습니다(단면의 꼭짓점 수·순서를 확인)");
    merge_body(q, bodies, {loft.Shape()});
  } else if (t == "surface_grid") {  // 점 격자 → NURBS 곡면 면
    const Json pts = q.value("points", Json::array());
    const int cols = q.value("columns", 0);
    if (cols < 2 || pts.size() < static_cast<std::size_t>(2 * cols) || pts.size() % static_cast<std::size_t>(cols) != 0)
      throw Error("invalid_param", "points 는 columns 의 배수 개(행 2개 이상)여야 합니다", {{"param", "points"}, {"columns", cols}});
    const int rows_n = static_cast<int>(pts.size()) / cols;
    TColgp_Array2OfPnt grid(1, rows_n, 1, cols);
    for (int r = 0; r < rows_n; ++r)
      for (int c2 = 0; c2 < cols; ++c2) {
        const Json& p = pts[static_cast<std::size_t>(r * cols + c2)];
        grid.SetValue(r + 1, c2 + 1, gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
      }
    GeomAPI_PointsToBSplineSurface mk(grid);
    if (!mk.IsDone()) throw Error("invalid_geometry", "곡면을 만들 수 없습니다(점이 겹치거나 꼬임)", {{"param", "points"}});
    Handle(Geom_Surface) surf = mk.Surface();
    BRepBuilderAPI_MakeFace face(surf, 1e-6);
    if (!face.IsDone()) throw Error("geometry_failed", "곡면 면을 만들 수 없습니다");
    bodies.push_back(face.Face());
  } else if (t == "offset") {
    need_body();
    for (TopoDS_Shape& b : bodies) {
      // 면을 연장해 만나게 잇는다(GeomAbs_Intersection): 상자를 d 만큼 오프셋하면 변이 2d 커진 상자가 된다
      BRepOffsetAPI_MakeOffsetShape mk;
      mk.PerformByJoin(b, number(q, "distance"), 1e-4, BRepOffset_Skin, false, false, GeomAbs_Intersection);
      if (!mk.IsDone() || mk.Shape().IsNull()) throw Error("geometry_failed", "오프셋에 실패했습니다");
      b = mk.Shape();
    }
  } else if (t == "thicken") {
    need_body();
    for (TopoDS_Shape& b : bodies) {
      if (TopExp_Explorer(b, TopAbs_SOLID).More()) throw Error("invalid_state", "솔리드에는 두께를 줄 수 없습니다(면·껍질만)");
      BRepOffsetAPI_MakeThickSolid mk;
      mk.MakeThickSolidBySimple(b, number(q, "thickness"));
      if (!mk.IsDone() || mk.Shape().IsNull()) throw Error("geometry_failed", "두께를 주지 못했습니다");
      TopoDS_Shape thick = mk.Shape();
      GProp_GProps props;
      BRepGProp::VolumeProperties(thick, props);
      if (props.Mass() < 0) thick.Reverse();  // 면의 법선 반대쪽으로 두께를 주면 안팎이 뒤집혀 나온다
      b = thick;
    }
  } else if (t == "make_solid") {
    need_body();
    BRepBuilderAPI_Sewing sew(q.value("tolerance", 1e-3));
    for (const TopoDS_Shape& b : bodies) sew.Add(b);
    sew.Perform();
    const TopoDS_Shape sewn = sew.SewedShape();
    std::vector<TopoDS_Shape> solids;
    for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
      TopoDS_Shell shell = TopoDS::Shell(ex.Current());
      if (!shell.Closed() && !BRep_Tool::IsClosed(shell)) continue;
      BRepBuilderAPI_MakeSolid mk(shell);
      if (!mk.IsDone()) continue;
      TopoDS_Solid solid = mk.Solid();
      GProp_GProps props;
      BRepGProp::VolumeProperties(solid, props);
      if (props.Mass() < 0) solid.Reverse();  // 안팎이 뒤집힌 껍질
      solids.push_back(solid);
    }
    if (solids.empty()) throw Error("geometry_failed", "닫힌 껍질이 없어 솔리드를 만들지 못했습니다(면이 모자라거나 공차 밖)");
    bodies = solids;
  } else if (t == "explode") {
    need_body();
    std::vector<TopoDS_Shape> faces;
    for (const TopoDS_Shape& b : bodies)
      for (TopExp_Explorer ex(b, TopAbs_FACE); ex.More(); ex.Next()) faces.push_back(ex.Current());
    if (faces.empty()) throw Error("geometry_failed", "분해할 면이 없습니다");
    bodies = faces;
  } else if (t == "split") {
    need_body();
    // 큰 평면 조각으로 자른다(바디의 경계 상자보다 넉넉히)
    Bnd_Box box;
    for (const TopoDS_Shape& b : bodies) BRepBndLib::Add(b, box);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double half = 2.0 * std::max({xmax - xmin, ymax - ymin, zmax - zmin, 1.0});
    const gp_Pnt p0 = pnt(q, "point");
    const gp_Pln pln(p0, dir(q, "normal", true));
    const TopoDS_Face tool = BRepBuilderAPI_MakeFace(pln, -half, half, -half, half).Face();
    ShapeList args, tools;
    for (const TopoDS_Shape& b : bodies) args.Append(b);
    tools.Append(tool);
    BRepAlgoAPI_Splitter op;
    op.SetArguments(args);
    op.SetTools(tools);
    op.Build();
    if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "분할에 실패했습니다");
    std::vector<TopoDS_Shape> pieces;
    for (TopExp_Explorer ex(op.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) pieces.push_back(ex.Current());
    if (pieces.empty())  // 솔리드가 아닌 형상(면)은 면 조각으로
      for (TopExp_Explorer ex(op.Shape(), TopAbs_FACE); ex.More(); ex.Next()) pieces.push_back(ex.Current());
    if (pieces.empty()) throw Error("geometry_failed", "분할 결과가 비었습니다");
    bodies = pieces;
  } else if (t == "fillet" || t == "chamfer") {
    need_body();
    const ShapeMap edges = current_map(bodies, TopAbs_EDGE);
    const double value = number(q, t == "fillet" ? "radius" : "distance");
    std::vector<TopoDS_Shape> picked;
    for (const Json& i : q.value("edges", Json::array())) picked.push_back(entity_of(edges, i.get<int>(), "edge"));
    if (picked.empty()) throw Error("missing_param", "필수 속성이 없습니다: edges", {{"param", "edges"}});
    for (TopoDS_Shape& b : bodies) {
      ShapeMap own;
      TopExp::MapShapes(b, TopAbs_EDGE, own);
      std::vector<TopoDS_Shape> mine;
      for (const TopoDS_Shape& e : picked)
        if (own.Contains(e)) mine.push_back(e);
      if (mine.empty()) continue;
      if (t == "fillet") {
        BRepFilletAPI_MakeFillet mk(b);
        for (const TopoDS_Shape& e : mine) mk.Add(value, TopoDS::Edge(e));
        mk.Build();
        if (!mk.IsDone()) throw Error("geometry_failed", "필렛을 만들지 못했습니다(반지름이 너무 크거나 모서리가 맞지 않음)");
        b = mk.Shape();
      } else {
        BRepFilletAPI_MakeChamfer mk(b);
        for (const TopoDS_Shape& e : mine) mk.Add(value, TopoDS::Edge(e));
        mk.Build();
        if (!mk.IsDone()) throw Error("geometry_failed", "챔퍼를 만들지 못했습니다(거리가 너무 크거나 모서리가 맞지 않음)");
        b = mk.Shape();
      }
    }
  } else if (t == "shell") {
    need_body();
    const ShapeMap faces = current_map(bodies, TopAbs_FACE);
    const double thickness = number(q, "thickness") * (q.value("outward", false) ? 1.0 : -1.0);
    std::vector<TopoDS_Shape> picked;
    for (const Json& i : q.value("faces", Json::array())) picked.push_back(entity_of(faces, i.get<int>(), "face"));
    if (picked.empty()) throw Error("missing_param", "필수 속성이 없습니다: faces", {{"param", "faces"}});
    for (TopoDS_Shape& b : bodies) {
      ShapeMap own;
      TopExp::MapShapes(b, TopAbs_FACE, own);
      ShapeList mine;
      for (const TopoDS_Shape& f : picked)
        if (own.Contains(f)) mine.Append(f);
      if (mine.IsEmpty()) continue;
      BRepOffsetAPI_MakeThickSolid mk;
      // 모서리는 둥글리지 않고 면을 연장해 만난다(GeomAbs_Intersection)
      mk.MakeThickSolidByJoin(b, mine, thickness, 1e-3, BRepOffset_Skin, false, false, GeomAbs_Intersection);
      if (!mk.IsDone()) throw Error("geometry_failed", "속을 비우지 못했습니다(두께가 너무 크거나 면이 맞지 않음)");
      // OCCT 는 오프셋이 안 되는 면(주기적인 한 장짜리 B-스플라인 등)에서 IsDone 인 채로 원래 모양이나 깨진 모양을 돌려주기도 한다
      GProp_GProps before_p, after_p;
      BRepGProp::VolumeProperties(b, before_p);
      BRepGProp::VolumeProperties(mk.Shape(), after_p);
      if (!BRepCheck_Analyzer(mk.Shape()).IsValid() || std::fabs(after_p.Mass() - before_p.Mass()) < 1e-9 * std::fabs(before_p.Mass()) ||
          after_p.Mass() <= 0.0)
        throw Error("geometry_failed", "속을 비우지 못했습니다(면을 오프셋할 수 없음 — 한 장짜리 닫힌 곡면이면 먼저 나누거나 다른 방법으로 만든다)");
      b = mk.Shape();
    }
  } else if (t == "pattern") {
    need_body();
    const int count = static_cast<int>(number(q, "count"));
    if (count < 2) throw Error("out_of_range", "개수는 2 이상이어야 합니다", {{"param", "count"}});
    const std::string kind = q.value("kind", std::string("linear"));
    std::vector<TopoDS_Shape> copies;
    for (int k = 1; k < count; ++k) {
      gp_Trsf trsf;
      if (kind == "linear") {
        if (!has(q, "vector")) throw Error("missing_param", "필수 속성이 없습니다: vector", {{"param", "vector"}});
        trsf.SetTranslation(gp_Vec(q["vector"][0].get<double>(), q["vector"][1].get<double>(), q["vector"][2].get<double>()) * k);
      } else {
        const double total = q.value("angle", 360.0);
        const double step = (total >= 360.0 ? total / count : total / (count - 1)) * kPi / 180.0;  // 한 바퀴면 끝이 처음과 겹치지 않게
        trsf.SetRotation(gp_Ax1(pnt(q, "point"), dir(q, "axis", true)), step * k);
      }
      for (const TopoDS_Shape& b : bodies) copies.push_back(transformed(b, trsf));
    }
    merge_body(q, bodies, copies);
  } else if (t == "translate") {
    if (!has(q, "vector")) throw Error("missing_param", "필수 속성이 없습니다: vector", {{"param", "vector"}});
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(q["vector"][0].get<double>(), q["vector"][1].get<double>(), q["vector"][2].get<double>()));
    transform_all(trsf);
  } else if (t == "rotate") {
    gp_Trsf trsf;
    trsf.SetRotation(gp_Ax1(pnt(q, "point"), dir(q, "axis", true)), number(q, "angle") * kPi / 180.0);
    transform_all(trsf);
  } else if (t == "mirror") {
    gp_Trsf trsf;
    trsf.SetMirror(gp_Ax2(pnt(q, "point"), dir(q, "normal", true)));
    transform_all(trsf);
  } else if (t == "scale") {
    gp_Trsf trsf;
    trsf.SetScale(pnt(q, "center"), number(q, "factor"));
    transform_all(trsf);
  } else if (t == "heal") {
    // 치유(GEO-10): ShapeFix(공차·방향·꼬임), 선택적으로 꿰매기와 짧은 모서리 제거
    need_body();
    for (TopoDS_Shape& b : bodies) {
      TopoDS_Shape s = b;
      if (q.value("sew", false)) {
        BRepBuilderAPI_Sewing sew(q.value("tolerance", 1e-3));
        sew.Add(s);
        sew.Perform();
        if (!sew.SewedShape().IsNull()) s = sew.SewedShape();
      }
      Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(s);
      if (has(q, "tolerance")) fix->SetPrecision(q["tolerance"].get<double>()), fix->SetMaxTolerance(10.0 * q["tolerance"].get<double>());
      fix->Perform();
      s = fix->Shape();
      if (has(q, "min_edge")) {
        Handle(ShapeFix_Wireframe) wf = new ShapeFix_Wireframe(s);
        wf->SetPrecision(q["min_edge"].get<double>());
        wf->FixSmallEdges();
        wf->FixWireGaps();
        s = wf->Shape();
      }
      if (s.IsNull()) throw Error("geometry_failed", "치유 결과가 비었습니다");
      // 꿰매기는 껍질을 돌려준다: 원래 솔리드였고 결과 껍질이 닫혀 있으면 다시 솔리드로
      if (TopExp_Explorer(b, TopAbs_SOLID).More() && !TopExp_Explorer(s, TopAbs_SOLID).More()) {
        std::vector<TopoDS_Shape> solids;
        for (TopExp_Explorer ex(s, TopAbs_SHELL); ex.More(); ex.Next()) {
          BRepBuilderAPI_MakeSolid mk(TopoDS::Shell(ex.Current()));
          if (!mk.IsDone()) continue;
          TopoDS_Solid solid = mk.Solid();
          GProp_GProps props;
          BRepGProp::VolumeProperties(solid, props);
          if (props.Mass() < 0) solid.Reverse();
          solids.push_back(solid);
        }
        if (!solids.empty()) s = compound_of(solids);
      }
      b = s;
    }
  } else if (t == "point") {
    bodies.push_back(BRepBuilderAPI_MakeVertex(pnt(q, "point")).Shape());
  } else if (t == "line") {
    if (!has(q, "start") || !has(q, "end")) throw Error("missing_param", "필수 속성이 없습니다: start, end", {{"param", "start"}});
    BRepBuilderAPI_MakeEdge mk(pnt(q, "start"), pnt(q, "end"));
    if (!mk.IsDone()) throw Error("invalid_geometry", "두 점이 같아 선을 만들 수 없습니다", {{"param", "end"}});
    bodies.push_back(mk.Shape());
  } else if (t == "arc") {
    if (!has(q, "start") || !has(q, "middle") || !has(q, "end")) throw Error("missing_param", "필수 속성이 없습니다: start, middle, end", {{"param", "start"}});
    GC_MakeArcOfCircle arc(pnt(q, "start"), pnt(q, "middle"), pnt(q, "end"));
    if (!arc.IsDone()) throw Error("invalid_geometry", "세 점이 한 직선 위에 있어 호를 만들 수 없습니다", {{"param", "middle"}});
    bodies.push_back(BRepBuilderAPI_MakeEdge(arc.Value()).Shape());
  } else if (t == "spline") {
    const Json pts = q.value("points", Json::array());
    if (pts.size() < 2) throw Error("out_of_range", "점이 2개 이상 있어야 합니다", {{"param", "points"}});
    TColgp_Array1OfPnt arr(1, static_cast<int>(pts.size()));
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) arr.SetValue(i + 1, gp_Pnt(pts[i][0].get<double>(), pts[i][1].get<double>(), pts[i][2].get<double>()));
    GeomAPI_PointsToBSpline mk(arr);
    if (!mk.IsDone()) throw Error("invalid_geometry", "스플라인을 만들 수 없습니다(점이 겹침)", {{"param", "points"}});
    bodies.push_back(BRepBuilderAPI_MakeEdge(mk.Curve()).Shape());
  } else if (t == "plane") {
    const double half = 0.5 * number(q, "size");
    bodies.push_back(BRepBuilderAPI_MakeFace(gp_Pln(pnt(q, "point"), dir(q, "normal")), -half, half, -half, half).Face());
  } else if (t == "imprint" || t == "share_topology") {
    need_body();
    ShapeList args;
    for (const TopoDS_Shape& b : bodies) args.Append(b);
    ShapeList tools;
    for (const Json& id : q.value("tools", Json::array())) {
      const PartShape& other = shape_of(a, id.get<Id>());
      if (other.shape.IsNull()) throw Error("invalid_state", "도구 파트에 형상이 없습니다", {{"object", id}});
      tools.Append(other.shape);
    }
    if (t == "imprint") {
      // 임프린트: 도구의 면·모서리로 이 바디의 면을 나눈다(Splitter). 바디의 부피는 그대로다
      if (tools.IsEmpty()) throw Error("missing_param", "필수 속성이 없습니다: tools", {{"param", "tools"}});
      BRepAlgoAPI_Splitter op;
      op.SetArguments(args), op.SetTools(tools), op.Build();
      if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "임프린트에 실패했습니다");
      std::vector<TopoDS_Shape> out;
      for (std::size_t i = 0; i < bodies.size(); ++i) {
        // 바디마다 결과에서 자기 조각들을 모은다(한 솔리드가 나뉘지 않으므로 Modified 가 하나 또는 자기 자신)
        const auto& mod = op.Modified(bodies[i]);
        if (mod.IsEmpty()) out.push_back(bodies[i]);
        else for (const TopoDS_Shape& m : mod) out.push_back(m);
      }
      bodies = out;
    } else {
      // 공유 토폴로지: 일반 합치기(General Fuse)로 맞닿은 바디들이 경계 면을 공유하게 한다
      for (const TopoDS_Shape& tl : tools) args.Append(tl);
      if (args.Extent() < 2) throw Error("invalid_state", "공유할 바디가 둘 이상 있어야 합니다(이 파트의 바디 또는 tools)");
      BRepAlgoAPI_BuilderAlgo op;
      op.SetArguments(args), op.SetNonDestructive(true), op.Build();
      if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "공유 토폴로지 생성에 실패했습니다");
      std::vector<TopoDS_Shape> out;
      for (TopExp_Explorer ex(op.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) out.push_back(ex.Current());
      if (out.empty())
        for (TopExp_Explorer ex(op.Shape(), TopAbs_FACE); ex.More(); ex.Next()) out.push_back(ex.Current());
      if (out.empty()) throw Error("geometry_failed", "공유 토폴로지 결과가 비었습니다");
      bodies = out;
    }
  } else if (t == "remove_fillet" || t == "remove_hole" || t == "remove_faces") {
    // 단순화(GEO-16): 면을 없애고 이웃 면을 늘여 메운다(Defeaturing)
    need_body();
    const ShapeMap faces = current_map(bodies, TopAbs_FACE);
    std::vector<TopoDS_Shape> picked;
    for (const Json& i : q.value("faces", Json::array())) picked.push_back(entity_of(faces, i.get<int>(), "face"));
    if (picked.empty()) throw Error("missing_param", "필수 속성이 없습니다: faces", {{"param", "faces"}});
    for (TopoDS_Shape& b : bodies) {
      ShapeMap own;
      TopExp::MapShapes(b, TopAbs_FACE, own);
      BRepAlgoAPI_Defeaturing op;
      op.SetShape(b);
      int n = 0;
      for (const TopoDS_Shape& f : picked)
        if (own.Contains(f)) op.AddFaceToRemove(f), ++n;
      if (!n) continue;
      op.SetRunParallel(false);
      op.Build();
      if (!op.IsDone() || op.HasErrors()) throw Error("geometry_failed", "면을 없애지 못했습니다(이웃 면을 늘여 메울 수 없음)");
      b = op.Shape();
    }
  } else if (t == "merge_faces" || t == "merge_edges") {
    need_body();
    const double ang = q.value("angle", 0.1) * kPi / 180.0;
    for (TopoDS_Shape& b : bodies) {
      ShapeUpgrade_UnifySameDomain unify(b, t == "merge_edges", t == "merge_faces", false);
      unify.SetAngularTolerance(ang);
      unify.Build();
      if (!unify.Shape().IsNull()) b = unify.Shape();
    }
  } else if (t == "midsurface") {
    // 중립면(GEO-17): 마주 보는 두 면 a·b 의 거리 d 를 재고 a 를 b 쪽으로 d/2 오프셋한다
    need_body();
    const ShapeMap faces = current_map(bodies, TopAbs_FACE);
    const Json fl = q.value("faces", Json::array());
    if (fl.size() != 2) throw Error("invalid_param", "faces 에는 마주 보는 두 면 번호를 적는다", {{"param", "faces"}});
    const TopoDS_Face fa = TopoDS::Face(entity_of(faces, fl[0].get<int>(), "face")), fb = TopoDS::Face(entity_of(faces, fl[1].get<int>(), "face"));
    BRepExtrema_DistShapeShape dist(fa, fb);
    if (!dist.IsDone() || dist.Value() <= 1e-9) throw Error("invalid_geometry", "두 면이 떨어져 있어야 합니다", {{"param", "faces"}});
    const double d = dist.Value();
    TopoDS_Shape mid;
    for (double sign : {-1.0, 1.0}) {
      BRepOffsetAPI_MakeOffsetShape mk;
      mk.PerformBySimple(fa, sign * 0.5 * d);
      if (!mk.IsDone() || mk.Shape().IsNull()) continue;
      BRepExtrema_DistShapeShape check(mk.Shape(), fb);
      if (check.IsDone() && std::fabs(check.Value() - 0.5 * d) < 1e-6 * std::max(1.0, d)) {
        mid = mk.Shape();
        break;
      }
    }
    if (mid.IsNull()) throw Error("geometry_failed", "중립면을 만들지 못했습니다(두 면이 오프셋 관계가 아님)");
    std::vector<TopoDS_Shape> out;
    if (q.value("keep", false)) out = bodies;
    for (TopExp_Explorer ex(mid, TopAbs_FACE); ex.More(); ex.Next()) out.push_back(ex.Current());
    bodies = out;
  } else if (t == "curve_trim" || t == "curve_extend" || t == "curve_split") {
    need_body();
    const TopoDS_Edge e = TopoDS::Edge(entity_of(current_map(bodies, TopAbs_EDGE), static_cast<int>(number(q, "edge")), "edge"));
    double u0, u1;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, u0, u1);
    if (c.IsNull()) throw Error("invalid_geometry", "곡선이 없는 모서리입니다", {{"param", "edge"}});
    if (t == "curve_trim") {
      const double s0 = q.value("start", 0.0), s1 = q.value("end", 1.0);
      if (s0 >= s1) throw Error("out_of_range", "start 는 end 보다 작아야 합니다", {{"param", "end"}});
      bodies.push_back(BRepBuilderAPI_MakeEdge(c, u0 + s0 * (u1 - u0), u0 + s1 * (u1 - u0)).Shape());
    } else if (t == "curve_split") {
      const double at = number(q, "at");
      if (at <= 0 || at >= 1) throw Error("out_of_range", "at 은 0 과 1 사이여야 합니다", {{"param", "at"}});
      const double um = u0 + at * (u1 - u0);
      bodies.push_back(BRepBuilderAPI_MakeEdge(c, u0, um).Shape());
      bodies.push_back(BRepBuilderAPI_MakeEdge(c, um, u1).Shape());
    } else {
      const double len = number(q, "length");
      const std::string at = q.value("at", std::string("end"));
      // 직선은 매개변수로, 그 밖의 곡선은 B-스플라인으로 바꿔 접선 방향으로 늘린다
      Handle(Geom_Line) line = Handle(Geom_Line)::DownCast(c);
      if (!line.IsNull()) {
        const double a0 = (at == "end") ? u0 : u0 - len, a1 = (at == "start") ? u1 : u1 + len;
        bodies.push_back(BRepBuilderAPI_MakeEdge(c, a0, a1).Shape());
      } else {
        Handle(Geom_BSplineCurve) bs = GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(c, u0, u1));
        for (const bool after : {false, true}) {
          if ((after && at == "start") || (!after && at == "end")) continue;
          gp_Pnt p;
          gp_Vec v;
          bs->D1(after ? bs->LastParameter() : bs->FirstParameter(), p, v);
          if (v.Magnitude() <= 0) throw Error("invalid_geometry", "접선을 구할 수 없습니다", {{"param", "edge"}});
          const gp_Pnt target = p.Translated(gp_Vec(v.Normalized()) * (after ? len : -len));
          GeomLib::ExtendCurveToPoint(bs, target, 1, after);
        }
        bodies.push_back(BRepBuilderAPI_MakeEdge(bs).Shape());
      }
    }
  } else if (t == "curve_join") {
    need_body();
    const ShapeMap edges = current_map(bodies, TopAbs_EDGE);
    const Json el = q.value("edges", Json::array());
    if (el.size() < 2) throw Error("out_of_range", "모서리가 2개 이상 있어야 합니다", {{"param", "edges"}});
    BRepBuilderAPI_MakeWire wire;
    for (const Json& i : el) wire.Add(TopoDS::Edge(entity_of(edges, i.get<int>(), "edge")));
    if (!wire.IsDone()) throw Error("invalid_geometry", "모서리들이 이어져 있지 않습니다", {{"param", "edges"}});
    GeomConvert_CompCurveToBSplineCurve comp;
    for (BRepTools_WireExplorer ex(wire.Wire()); ex.More(); ex.Next()) {
      double a, b;
      Handle(Geom_Curve) c = BRep_Tool::Curve(ex.Current(), a, b);
      Handle(Geom_TrimmedCurve) tc = new Geom_TrimmedCurve(c, a, b);
      if (ex.Current().Orientation() == TopAbs_REVERSED) tc->Reverse();
      if (!comp.Add(tc, 1e-6, true)) throw Error("invalid_geometry", "곡선을 하나로 잇지 못했습니다", {{"param", "edges"}});
    }
    bodies.push_back(BRepBuilderAPI_MakeEdge(comp.BSplineCurve()).Shape());
  } else if (t == "curve_project") {
    need_body();
    const TopoDS_Edge e = TopoDS::Edge(entity_of(current_map(bodies, TopAbs_EDGE), static_cast<int>(number(q, "edge")), "edge"));
    const TopoDS_Face f = TopoDS::Face(entity_of(current_map(bodies, TopAbs_FACE), static_cast<int>(number(q, "face")), "face"));
    BRepAlgo_NormalProjection proj(f);
    proj.Add(e);
    try {
      proj.Build();
    } catch (const Standard_Failure& ex) {
      throw Error("geometry_failed", std::string("법선 투영에 실패했습니다(곡면의 극·이음매 근처일 수 있음): ") + (ex.GetMessageString() ? ex.GetMessageString() : ""));
    }
    if (!proj.IsDone() || proj.Projection().IsNull() || !TopExp_Explorer(proj.Projection(), TopAbs_EDGE).More())
      throw Error("geometry_failed", "투영 결과가 비었습니다(모서리가 면 위에 놓이지 않음)");
    for (TopExp_Explorer ex(proj.Projection(), TopAbs_EDGE); ex.More(); ex.Next()) bodies.push_back(ex.Current());
  } else if (t == "curve_intersect") {
    need_body();
    const ShapeMap faces = current_map(bodies, TopAbs_FACE);
    const TopoDS_Face fa = TopoDS::Face(entity_of(faces, static_cast<int>(number(q, "face_a")), "face")), fb = TopoDS::Face(entity_of(faces, static_cast<int>(number(q, "face_b")), "face"));
    BRepAlgoAPI_Section sec(fa, fb, false);
    sec.Approximation(true);
    sec.Build();
    if (!sec.IsDone() || !TopExp_Explorer(sec.Shape(), TopAbs_EDGE).More()) throw Error("geometry_failed", "두 면이 만나지 않습니다");
    for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) bodies.push_back(ex.Current());
  } else if (t == "curve_offset") {
    need_body();
    const TopoDS_Edge e = TopoDS::Edge(entity_of(current_map(bodies, TopAbs_EDGE), static_cast<int>(number(q, "edge")), "edge"));
    BRepBuilderAPI_MakeWire wire(e);
    BRepOffsetAPI_MakeOffset mk;
    if (has(q, "normal")) mk.Init(BRepBuilderAPI_MakeFace(gp_Pln(BRep_Tool::Pnt(TopExp::FirstVertex(e)), dir(q, "normal"))).Face(), GeomAbs_Arc, true);
    else mk.Init(GeomAbs_Arc, true);
    mk.AddWire(wire.Wire());
    mk.Perform(number(q, "distance"));
    if (!mk.IsDone() || mk.Shape().IsNull()) throw Error("geometry_failed", "곡선 오프셋에 실패했습니다(평면 곡선이 아니거나 법선이 필요)");
    for (TopExp_Explorer ex(mk.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) bodies.push_back(ex.Current());
  } else if (t == "face_fill") {
    need_body();
    const ShapeMap edges = current_map(bodies, TopAbs_EDGE);
    const Json el = q.value("edges", Json::array());
    if (el.empty()) throw Error("missing_param", "필수 속성이 없습니다: edges", {{"param", "edges"}});
    BRepBuilderAPI_MakeWire wire;
    std::vector<TopoDS_Shape> used;
    for (const Json& i : el) {
      const TopoDS_Shape e = entity_of(edges, i.get<int>(), "edge");
      wire.Add(TopoDS::Edge(e));
      used.push_back(e);
    }
    if (!wire.IsDone() || !wire.Wire().Closed()) throw Error("invalid_geometry", "모서리들이 닫힌 고리를 이루지 않습니다", {{"param", "edges"}});
    TopoDS_Face filled;
    BRepBuilderAPI_MakeFace planar(wire.Wire(), true);
    if (planar.IsDone()) {
      filled = planar.Face();
    } else {
      BRepFill_Filling fill;
      for (BRepTools_WireExplorer ex(wire.Wire()); ex.More(); ex.Next()) fill.Add(ex.Current(), GeomAbs_C0);
      fill.Build();
      if (!fill.IsDone()) throw Error("geometry_failed", "면을 채우지 못했습니다");
      filled = fill.Face();
    }
    // 고리로 쓴 독립 모서리 바디(선·호·스플라인 피처)는 면에 흡수된다 — 남겨 두면 같은 자리의 모서리가 겹쳐 비다양체·측정 오류가 된다
    std::vector<TopoDS_Shape> kept;
    for (const TopoDS_Shape& b : bodies) {
      bool consumed = false;
      if (b.ShapeType() == TopAbs_EDGE)
        for (const TopoDS_Shape& u : used) consumed = consumed || b.IsSame(u);
      if (!consumed) kept.push_back(b);
    }
    bodies = kept;
    bodies.push_back(filled);
  } else if (t == "face_extend") {
    need_body();
    const TopoDS_Face f = TopoDS::Face(entity_of(current_map(bodies, TopAbs_FACE), static_cast<int>(number(q, "face")), "face"));
    BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane) throw Error("invalid_geometry", "평면 면만 늘릴 수 있습니다", {{"param", "face"}});
    BRepOffsetAPI_MakeOffset mk(f, GeomAbs_Intersection);
    mk.Perform(number(q, "length"));
    if (!mk.IsDone() || mk.Shape().IsNull()) throw Error("geometry_failed", "면을 늘리지 못했습니다");
    TopoDS_Wire outer;
    for (TopExp_Explorer ex(mk.Shape(), TopAbs_WIRE); ex.More(); ex.Next()) outer = TopoDS::Wire(ex.Current());
    if (outer.IsNull()) throw Error("geometry_failed", "면을 늘리지 못했습니다");
    BRepBuilderAPI_MakeFace mf(surf.Plane(), outer, true);
    if (!mf.IsDone()) throw Error("geometry_failed", "늘린 둘레로 면을 만들지 못했습니다");
    bodies.push_back(mf.Face());
  } else if (t == "face_trim") {
    need_body();
    const TopoDS_Face f = TopoDS::Face(entity_of(current_map(bodies, TopAbs_FACE), static_cast<int>(number(q, "face")), "face"));
    // 반공간(법선 쪽)과의 공통 부분
    Bnd_Box box;
    BRepBndLib::Add(f, box);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double size = 4.0 * std::max({xmax - xmin, ymax - ymin, zmax - zmin, 1.0});
    const gp_Dir n = dir(q, "normal", true);
    const gp_Pnt p0 = pnt(q, "point");
    const TopoDS_Shape half = BRepPrimAPI_MakeBox(gp_Ax2(p0.Translated(gp_Vec(n) * -0.0), n), size, size, size).Shape();
    gp_Trsf center;  // 상자를 평면 위에서 중심에 두도록 옆으로 옮긴다
    center.SetTranslation(gp_Vec(gp_Ax2(p0, n).XDirection()) * (-0.5 * size) + gp_Vec(gp_Ax2(p0, n).YDirection()) * (-0.5 * size));
    BRepAlgoAPI_Common op(f, transformed(half, center));
    if (!op.IsDone() || !TopExp_Explorer(op.Shape(), TopAbs_FACE).More()) throw Error("geometry_failed", "자른 결과가 비었습니다(남길 쪽에 면이 없음)");
    for (TopExp_Explorer ex(op.Shape(), TopAbs_FACE); ex.More(); ex.Next()) bodies.push_back(ex.Current());
  } else if (t == "face_delete" || t == "face_replace") {
    need_body();
    const ShapeMap faces = current_map(bodies, TopAbs_FACE);
    BRepTools_ReShape reshape;
    std::vector<TopoDS_Shape> to_delete;
    if (t == "face_delete") {
      const Json fl = q.value("faces", Json::array());
      if (fl.empty()) throw Error("missing_param", "필수 속성이 없습니다: faces", {{"param", "faces"}});
      for (const Json& i : fl) to_delete.push_back(entity_of(faces, i.get<int>(), "face")), reshape.Remove(to_delete.back());
    } else {
      // 면 대체(GEO-28): 바꿀 면이 평면이고 대신 쓸 면도 평면이면 솔리드를 그 평면까지 늘리거나 잘라 닫힌 솔리드를 유지한다.
      // (바꿀 면을 바깥쪽으로 길게 뽑아 더한 뒤, 새 평면의 안쪽 반공간과의 공통 부분을 취한다)
      const TopoDS_Face old_face = TopoDS::Face(entity_of(faces, static_cast<int>(number(q, "face")), "face"));
      const TopoDS_Face new_face = TopoDS::Face(entity_of(faces, static_cast<int>(number(q, "with")), "face"));
      BRepAdaptor_Surface so(old_face), sn(new_face);
      bool done = false;
      if (so.GetType() == GeomAbs_Plane && sn.GetType() == GeomAbs_Plane) {
        for (TopoDS_Shape& b : bodies) {
          if (!TopExp_Explorer(b, TopAbs_SOLID).More()) continue;
          ShapeMap own;
          TopExp::MapShapes(b, TopAbs_FACE, own);
          if (!own.Contains(old_face)) continue;
          Bnd_Box box;
          BRepBndLib::Add(b, box);
          double xmin, ymin, zmin, xmax, ymax, zmax;
          box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
          const double size = 4.0 * std::max({xmax - xmin, ymax - ymin, zmax - zmin, 1.0});
          // 바꿀 면의 바깥 법선: 솔리드의 무게중심이 반대쪽에 있게
          GProp_GProps props;
          BRepGProp::VolumeProperties(b, props);
          const gp_Pnt cg = props.CentreOfMass();
          gp_Dir n_old = so.Plane().Axis().Direction();
          if (gp_Vec(so.Plane().Location(), cg).Dot(gp_Vec(n_old)) > 0) n_old.Reverse();
          gp_Dir n_new = sn.Plane().Axis().Direction();
          if (gp_Vec(sn.Plane().Location(), cg).Dot(gp_Vec(n_new)) > 0) n_new.Reverse();
          // 바깥으로 뽑은 기둥을 더하고
          BRepPrimAPI_MakePrism prism(old_face, gp_Vec(n_old) * size);
          BRepAlgoAPI_Fuse fuse(b, prism.Shape());
          if (!fuse.IsDone()) throw Error("geometry_failed", "면을 늘리지 못했습니다");
          // 새 평면의 안쪽 반공간(큰 상자)과의 공통 부분
          const gp_Pnt p0 = sn.Plane().Location();
          const gp_Ax2 ax(p0, n_new.Reversed());
          gp_Trsf shift;
          shift.SetTranslation(gp_Vec(ax.XDirection()) * (-0.5 * size) + gp_Vec(ax.YDirection()) * (-0.5 * size));
          const TopoDS_Shape half = transformed(BRepPrimAPI_MakeBox(ax, size, size, size).Shape(), shift);
          BRepAlgoAPI_Common common(fuse.Shape(), half);
          if (!common.IsDone() || !TopExp_Explorer(common.Shape(), TopAbs_SOLID).More()) throw Error("geometry_failed", "면을 대체한 결과가 비었습니다");
          ShapeUpgrade_UnifySameDomain unify(common.Shape(), true, true, false);
          unify.Build();
          TopoDS_Shape r = unify.Shape();
          std::vector<TopoDS_Shape> solids;
          for (TopExp_Explorer ex(r, TopAbs_SOLID); ex.More(); ex.Next()) solids.push_back(ex.Current());
          b = solids.size() == 1 ? solids[0] : compound_of(solids);
          done = true;
        }
      }
      if (done) {  // 대신 쓴 면이 따로 있는 바디(평면 피처)였으면 쓰고 없앤다
        bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [&](const TopoDS_Shape& b) { return b.IsSame(new_face); }), bodies.end());
        return;
      }
      reshape.Replace(old_face, new_face);
    }
    for (TopoDS_Shape& b : bodies) {
      // 면 삭제(GEO-28): 솔리드면 먼저 이웃 면을 늘여 메운다(Defeaturing). 안 되면 면만 빼서 열린 껍질로 둔다
      if (t == "face_delete" && TopExp_Explorer(b, TopAbs_SOLID).More()) {
        ShapeMap own;
        TopExp::MapShapes(b, TopAbs_FACE, own);
        BRepAlgoAPI_Defeaturing op;
        op.SetShape(b);
        int n = 0;
        for (const TopoDS_Shape& f : to_delete)
          if (own.Contains(f)) op.AddFaceToRemove(f), ++n;
        if (n) {
          op.SetRunParallel(false);
          op.Build();
          if (op.IsDone() && !op.HasErrors() && TopExp_Explorer(op.Shape(), TopAbs_SOLID).More()) {
            ShapeMap after;
            TopExp::MapShapes(op.Shape(), TopAbs_FACE, after);
            if (after.Extent() < own.Extent()) {  // 실제로 면이 줄었을 때만(이웃 면으로 메운 결과). 못 메우면 아래에서 면만 뺀다
              b = op.Shape();
              continue;
            }
          }
        }
      }
      TopoDS_Shape s = reshape.Apply(b);
      if (t == "face_replace") {
        BRepBuilderAPI_Sewing sew(q.value("tolerance", 1e-3));
        sew.Add(s);
        sew.Perform();
        if (!sew.SewedShape().IsNull()) s = sew.SewedShape();
      }
      if (s.IsNull() || !TopExp_Explorer(s, TopAbs_FACE).More()) throw Error("geometry_failed", "면을 지운 결과가 비었습니다");
      // 솔리드였던 것이 면을 잃으면 열린 껍질이 된다: 껍질·면만 남긴다
      std::vector<TopoDS_Shape> parts;
      for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) {
        BRepCheck_Analyzer ana(ex.Current());
        if (ana.IsValid()) parts.push_back(ex.Current());
        else for (TopExp_Explorer sh(ex.Current(), TopAbs_SHELL); sh.More(); sh.Next()) parts.push_back(sh.Current());
      }
      if (parts.empty()) {
        for (TopExp_Explorer ex(s, TopAbs_SHELL, TopAbs_SOLID); ex.More(); ex.Next()) parts.push_back(ex.Current());
        for (TopExp_Explorer ex(s, TopAbs_FACE, TopAbs_SHELL); ex.More(); ex.Next()) parts.push_back(ex.Current());
      }
      b = parts.size() == 1 ? parts[0] : compound_of(parts);
    }
  } else if (t == "hole") {
    need_body();
    if (!has(q, "point")) throw Error("missing_param", "필수 속성이 없습니다: point", {{"param", "point"}});
    Bnd_Box box;
    for (const TopoDS_Shape& b : bodies) BRepBndLib::Add(b, box);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double through = 2.0 * std::max({xmax - xmin, ymax - ymin, zmax - zmin, 1.0});
    // 뚫는 방향(기본 +z)으로 depth 만큼. 면 위에서 시작하면 경계가 겹쳐 불리언이 약하므로 면 밖에서 조금 앞서 시작한다
    const gp_Dir d = dir(q, "direction", false);
    const double depth = has(q, "depth") ? q["depth"].get<double>() : through;
    const double lead = has(q, "depth") ? 0.01 * depth : 0.5 * through;  // 관통이면 바디 밖 충분히 앞에서
    const gp_Pnt start = pnt(q, "point").Translated(gp_Vec(d) * -lead);
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(start, d), 0.5 * number(q, "diameter"), depth + lead).Shape();
    Json mq = q;
    mq["merge"] = "cut";
    merge_body(mq, bodies, {cyl});
  } else if (t == "pocket" || t == "boss") {
    need_body();
    const TopoDS_Face profile = profile_face(a, q, bodies);
    gp_Dir d = has(q, "direction") ? dir(q, "direction") : face_normal(profile);
    const double len = number(q, t == "pocket" ? "depth" : "height");
    if (t == "pocket" && !has(q, "direction")) d.Reverse();  // 파기: 면의 법선 반대(안쪽)로
    TopoDS_Shape made = BRepPrimAPI_MakePrism(profile, gp_Vec(d) * len).Shape();
    if (t == "pocket") {  // 면 위에서 시작하면 경계가 겹쳐 불리언이 약하다: 바깥쪽으로 조금 더 민다
      gp_Trsf tr;
      tr.SetTranslation(gp_Vec(d) * (-0.01 * len));
      made = BRepPrimAPI_MakePrism(transformed(profile, tr), gp_Vec(d) * (1.01 * len)).Shape();
    }
    Json mq = q;
    mq["merge"] = t == "pocket" ? "cut" : "fuse";
    merge_body(mq, bodies, {made});
  } else if (t == "rib") {
    need_body();
    const Json path = q.value("path", Json::array());
    if (path.size() < 2) throw Error("out_of_range", "경로에는 점이 2개 이상 있어야 합니다", {{"param", "path"}});
    const gp_Dir up = dir(q, "direction", true);
    const double th = number(q, "thickness"), h = number(q, "height");
    // 경로 조각마다 두께 방향으로 th, 높이 방향으로 h 인 판을 만들어 합친다
    std::vector<TopoDS_Shape> plates;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      const gp_Pnt p0(path[i][0].get<double>(), path[i][1].get<double>(), path[i][2].get<double>());
      const gp_Pnt p1(path[i + 1][0].get<double>(), path[i + 1][1].get<double>(), path[i + 1][2].get<double>());
      const gp_Vec along(p0, p1);
      if (along.Magnitude() <= 0) throw Error("invalid_geometry", "경로의 점이 겹칩니다", {{"param", "path"}});
      gp_Dir n = has(q, "normal") ? dir(q, "normal") : gp_Dir(along.Crossed(gp_Vec(up)));
      const gp_Vec half = gp_Vec(n) * (0.5 * th);
      BRepBuilderAPI_MakePolygon poly(p0.Translated(-half), p1.Translated(-half), p1.Translated(half), p0.Translated(half), true);
      plates.push_back(BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(poly.Wire(), true).Face(), gp_Vec(up) * h).Shape());
    }
    Json mq = q;
    mq["merge"] = "fuse";
    merge_body(mq, bodies, plates);
  } else if (t == "draft") {
    need_body();
    const ShapeMap faces = current_map(bodies, TopAbs_FACE);
    std::vector<TopoDS_Shape> picked;
    for (const Json& i : q.value("faces", Json::array())) picked.push_back(entity_of(faces, i.get<int>(), "face"));
    if (picked.empty()) throw Error("missing_param", "필수 속성이 없습니다: faces", {{"param", "faces"}});
    const gp_Dir pull = dir(q, "direction", true);
    const gp_Pln neutral(pnt(q, "point"), has(q, "normal") ? dir(q, "normal") : pull);
    const double ang = number(q, "angle") * kPi / 180.0;
    for (TopoDS_Shape& b : bodies) {
      ShapeMap own;
      TopExp::MapShapes(b, TopAbs_FACE, own);
      BRepOffsetAPI_DraftAngle mk(b);
      int n = 0;
      for (const TopoDS_Shape& f : picked) {
        if (!own.Contains(f)) continue;
        mk.Add(TopoDS::Face(f), pull, ang, neutral);
        if (!mk.AddDone()) throw Error("geometry_failed", "이 면은 기울일 수 없습니다(뽑는 방향·중립 평면을 확인)");
        ++n;
      }
      if (!n) continue;
      mk.Build();
      if (!mk.IsDone()) throw Error("geometry_failed", "기울기를 주지 못했습니다");
      b = mk.Shape();
    }
  } else {
    throw Error("not_available", "아직 구현하지 않은 피처입니다: " + t);
  }
}

// 파트의 형상. 피처가 바뀌었으면 다시 계산한다.
// 영속 이름표(D9)용: 엔티티의 기하 서명. 면은 바탕 곡면(평면: 법선·거리, 원통: 축·반지름 …), 모서리는 바탕 곡선(직선: 방향·점, 원: 중심·축·반지름 …),
// 꼭짓점은 위치, 솔리드는 체적·중심. 같은 바탕 곡면·곡선이면 잘리거나 늘어나도(TShape 가 바뀌어도) 같은 엔티티로 보고 이름을 잇는다.
struct EntitySignature {
  int kind = 0;
  std::vector<double> params;  // 바탕 기하의 매개변수(부호 없는 방향은 정규화)
  double c[3] = {0, 0, 0};     // 중심(같은 바탕 기하가 여럿일 때 가장 가까운 것을 고른다)
  double measure = 0;
};
void unsigned_dir(const gp_Dir& d, std::vector<double>& out) {  // 방향의 부호를 없앤다(법선·축의 뒤집힘 무시)
  double v[3] = {d.X(), d.Y(), d.Z()};
  int first = 0;
  while (first < 3 && std::fabs(v[first]) < 1e-12) ++first;
  const double sgn = first < 3 && v[first] < 0 ? -1.0 : 1.0;
  for (double x : v) out.push_back(sgn * x);
}
EntitySignature signature_of(const TopoDS_Shape& s, TopAbs_ShapeEnum kind) {
  EntitySignature sig;
  GProp_GProps g;
  if (kind == TopAbs_VERTEX) {
    const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(s));
    sig.c[0] = q.X(), sig.c[1] = q.Y(), sig.c[2] = q.Z();
    sig.params = {q.X(), q.Y(), q.Z()};
    return sig;
  }
  if (kind == TopAbs_SOLID) {
    BRepGProp::VolumeProperties(s, g);
  } else if (kind == TopAbs_FACE) {
    BRepGProp::SurfaceProperties(s, g);
    const BRepAdaptor_Surface surf(TopoDS::Face(s));
    sig.kind = static_cast<int>(surf.GetType());
    if (surf.GetType() == GeomAbs_Plane) {
      const gp_Pln pl = surf.Plane();
      unsigned_dir(pl.Axis().Direction(), sig.params);
      const gp_Dir n = pl.Axis().Direction();
      const double d = n.X() * pl.Location().X() + n.Y() * pl.Location().Y() + n.Z() * pl.Location().Z();
      sig.params.push_back(sig.params[0] * n.X() + sig.params[1] * n.Y() + sig.params[2] * n.Z() < 0 ? -d : d);
    } else if (surf.GetType() == GeomAbs_Cylinder) {
      const gp_Cylinder cy = surf.Cylinder();
      unsigned_dir(cy.Axis().Direction(), sig.params);
      const gp_Dir ax = cy.Axis().Direction();
      const gp_Pnt o = cy.Location();
      const double t = o.X() * ax.X() + o.Y() * ax.Y() + o.Z() * ax.Z();  // 축 위의 점을 축에 수직인 성분으로
      sig.params.insert(sig.params.end(), {o.X() - t * ax.X(), o.Y() - t * ax.Y(), o.Z() - t * ax.Z(), cy.Radius()});
    } else if (surf.GetType() == GeomAbs_Sphere) {
      const gp_Pnt o = surf.Sphere().Location();
      sig.params = {o.X(), o.Y(), o.Z(), surf.Sphere().Radius()};
    } else if (surf.GetType() == GeomAbs_Cone) {
      const gp_Cone co = surf.Cone();
      const gp_Pnt apex = co.Apex();
      unsigned_dir(co.Axis().Direction(), sig.params);
      sig.params.insert(sig.params.end(), {apex.X(), apex.Y(), apex.Z(), std::fabs(co.SemiAngle())});
    } else if (surf.GetType() == GeomAbs_Torus) {
      const gp_Torus to = surf.Torus();
      unsigned_dir(to.Axis().Direction(), sig.params);
      sig.params.insert(sig.params.end(), {to.Location().X(), to.Location().Y(), to.Location().Z(), to.MajorRadius(), to.MinorRadius()});
    } else {  // 자유 곡면: 넓이와 중심으로
      sig.params = {g.Mass()};
    }
  } else {  // 모서리
    BRepGProp::LinearProperties(s, g);
    const TopoDS_Edge e = TopoDS::Edge(s);
    if (BRep_Tool::Degenerated(e)) {
      sig.kind = -1;
    } else {
      const BRepAdaptor_Curve cv(e);
      sig.kind = static_cast<int>(cv.GetType());
      if (cv.GetType() == GeomAbs_Line) {
        const gp_Lin ln = cv.Line();
        unsigned_dir(ln.Direction(), sig.params);
        const gp_Dir d = ln.Direction();
        const gp_Pnt o = ln.Location();
        const double t = o.X() * d.X() + o.Y() * d.Y() + o.Z() * d.Z();
        sig.params.insert(sig.params.end(), {o.X() - t * d.X(), o.Y() - t * d.Y(), o.Z() - t * d.Z()});
      } else if (cv.GetType() == GeomAbs_Circle) {
        const gp_Circ ci = cv.Circle();
        unsigned_dir(ci.Axis().Direction(), sig.params);
        sig.params.insert(sig.params.end(), {ci.Location().X(), ci.Location().Y(), ci.Location().Z(), ci.Radius()});
      } else if (cv.GetType() == GeomAbs_Ellipse) {
        const gp_Elips el = cv.Ellipse();
        unsigned_dir(el.Axis().Direction(), sig.params);
        sig.params.insert(sig.params.end(), {el.Location().X(), el.Location().Y(), el.Location().Z(), el.MajorRadius(), el.MinorRadius()});
      } else {
        sig.params = {g.Mass()};
      }
    }
  }
  sig.measure = g.Mass();
  const gp_Pnt q = g.Mass() > 0 ? g.CentreOfMass() : gp_Pnt(0, 0, 0);
  sig.c[0] = q.X(), sig.c[1] = q.Y(), sig.c[2] = q.Z();
  return sig;
}
// 같은 바탕 기하인지(매개변수가 허용 오차 안)
bool same_geometry(const EntitySignature& a, const EntitySignature& b, double tol) {
  if (a.kind != b.kind || a.params.size() != b.params.size()) return false;
  for (std::size_t i = 0; i < a.params.size(); ++i)
    if (std::fabs(a.params[i] - b.params[i]) > tol * std::max({1.0, std::fabs(a.params[i]), std::fabs(b.params[i])})) return false;
  return true;
}
double center_distance(const EntitySignature& a, const EntitySignature& b) {
  return std::sqrt((a.c[0] - b.c[0]) * (a.c[0] - b.c[0]) + (a.c[1] - b.c[1]) * (a.c[1] - b.c[1]) + (a.c[2] - b.c[2]) * (a.c[2] - b.c[2]));
}

const PartShape& shape_of(App& a, Id part) {
  std::set<Id> stack;
  const std::string digest = digest_of(a, part, stack);
  PartShape& ps = store(a).parts[part];
  if (ps.digest == digest) return ps;
  PartShape fresh;
  fresh.digest = digest;
  std::vector<TopoDS_Shape> bodies;
  const Object& p = a.model().get(part);
  const Id rollback = has(p.props, "rollback") ? p.props["rollback"].get<Id>() : 0;
  bool failed = false, past = false;
  // 이름표 추적: 종류별 (엔티티 → 이름). 피처마다 새 엔티티에 "f<피처>:<종류>:<순번>" 을 붙인다
  static const std::pair<TopAbs_ShapeEnum, const char*> kinds[] = {{TopAbs_SOLID, "solid"}, {TopAbs_FACE, "face"}, {TopAbs_EDGE, "edge"}, {TopAbs_VERTEX, "vertex"}};
  std::map<std::string, NCollection_DataMap<TopoDS_Shape, std::string, TopTools_ShapeMapHasher>> named;
  double scale = 1.0;
  auto name_entities = [&](Id feature) {
    if (bodies.empty()) {
      for (const auto& [kind, type] : kinds) named[type].Clear();
      return;
    }
    const TopoDS_Shape all = compound_of(bodies);
    Bnd_Box box;
    BRepBndLib::Add(all, box);
    if (!box.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      scale = std::max({1.0, x1 - x0, y1 - y0, z1 - z0});
    }
    for (const auto& [kind, type] : kinds) {
      ShapeMap now;
      TopExp::MapShapes(all, kind, now);
      auto& prev = named[type];
      NCollection_DataMap<TopoDS_Shape, std::string, TopTools_ShapeMapHasher> next;
      // 1) 그대로 살아남은 엔티티는 이름 유지
      std::vector<int> unnamed;
      for (int i = 1; i <= now.Extent(); ++i) {
        const std::string* found = prev.Seek(now(i));
        if (found) next.Bind(now(i), *found);
        else unnamed.push_back(i);
      }
      // 2) 사라진 엔티티와 같은 기하 서명이면 이름을 잇는다(TShape 가 바뀐 경우)
      std::vector<std::pair<EntitySignature, std::string>> gone;
      for (NCollection_DataMap<TopoDS_Shape, std::string, TopTools_ShapeMapHasher>::Iterator it(prev); it.More(); it.Next())
        if (!now.Contains(it.Key())) gone.emplace_back(signature_of(it.Key(), kind), it.Value());
      std::set<std::string> used;
      for (NCollection_DataMap<TopoDS_Shape, std::string, TopTools_ShapeMapHasher>::Iterator it(next); it.More(); it.Next()) used.insert(it.Value());
      std::vector<int> still;
      for (int i : unnamed) {
        const EntitySignature sig = signature_of(now(i), kind);
        int best = -1;
        double best_d = 1e300;
        for (std::size_t gi = 0; gi < gone.size(); ++gi) {
          if (used.count(gone[gi].second) || !same_geometry(gone[gi].first, sig, 1e-7 * scale)) continue;
          const double d = center_distance(gone[gi].first, sig);
          if (d < best_d) best_d = d, best = static_cast<int>(gi);
        }
        if (best >= 0) next.Bind(now(i), gone[static_cast<std::size_t>(best)].second), used.insert(gone[static_cast<std::size_t>(best)].second);
        else still.push_back(i);
      }
      // 3) 나머지는 이 피처가 만든 새 엔티티: 순번은 훑은 순서
      int n = 0;
      for (int i : still) next.Bind(now(i), "f" + std::to_string(feature) + ":" + type + ":" + std::to_string(++n));
      prev = next;
    }
  };
  for (const Object* f : a.model().children(part, "feature")) {
    Json st{{"feature", f->id}, {"name", f->name}, {"type", subtype_of(*f)}};
    if (past) {
      st["state"] = "rolled_back";
    } else if (f->suppressed) {
      st["state"] = "suppressed";
    } else if (failed) {
      st["state"] = "skipped";
    } else {
      try {
        apply(a, *f, bodies);
        st["state"] = "ok";
        name_entities(f->id);
      } catch (const Error& e) {
        st["state"] = "error", st["code"] = e.code(), st["message"] = e.what();
        failed = true;
      } catch (const Standard_Failure& e) {
        st["state"] = "error", st["code"] = "geometry_failed", st["message"] = e.what();
        failed = true;
      }
    }
    fresh.status.push_back(std::move(st));
    if (rollback && f->id == rollback) past = true;
  }
  fresh.ok = !failed;
  if (!bodies.empty()) {
    fresh.shape = compound_of(bodies);
    TopExp::MapShapes(fresh.shape, TopAbs_SOLID, fresh.solids);
    TopExp::MapShapes(fresh.shape, TopAbs_FACE, fresh.faces);
    TopExp::MapShapes(fresh.shape, TopAbs_EDGE, fresh.edges);
    TopExp::MapShapes(fresh.shape, TopAbs_VERTEX, fresh.vertices);
    for (const auto& [kind, type] : kinds) {
      const ShapeMap& m = kind == TopAbs_SOLID ? fresh.solids : kind == TopAbs_FACE ? fresh.faces : kind == TopAbs_EDGE ? fresh.edges : fresh.vertices;
      auto& list = fresh.names[type];
      for (int i = 1; i <= m.Extent(); ++i) {
        const std::string* nm = named[type].Seek(m(i));
        list.push_back(nm ? *nm : std::string());
        if (nm) fresh.index_of_name[type][*nm] = i;
      }
    }
  }
  ps = std::move(fresh);
  return ps;
}

std::string entity_name_of(App& a, Id part, const std::string& type, int index) {
  const PartShape& ps = shape_of(a, part);
  auto it = ps.names.find(type);
  if (it == ps.names.end() || index < 1 || index > static_cast<int>(it->second.size())) return std::string();
  return it->second[static_cast<std::size_t>(index - 1)];
}

int entity_index_of(App& a, Id part, const std::string& type, const std::string& name) {
  const PartShape& ps = shape_of(a, part);
  auto it = ps.index_of_name.find(type);
  if (it == ps.index_of_name.end()) return 0;
  auto ni = it->second.find(name);
  return ni == it->second.end() ? 0 : ni->second;
}

const PartShape& solid_shape(App& a, const Object& part) {
  const PartShape& ps = shape_of(a, part.id);
  if (ps.shape.IsNull()) throw Error("invalid_state", "파트에 형상이 없습니다", {{"object", part.id}});
  return ps;
}

const ShapeMap& map_of(const PartShape& ps, const std::string& type) {
  if (type == "solid") return ps.solids;
  if (type == "face") return ps.faces;
  if (type == "edge") return ps.edges;
  if (type == "vertex") return ps.vertices;
  throw Error("out_of_range", "엔티티 종류는 solid, face, edge, vertex 가운데 하나여야 합니다", {{"param", "type"}});
}
TopAbs_ShapeEnum enum_of(const std::string& type) {
  return type == "solid" ? TopAbs_SOLID : type == "face" ? TopAbs_FACE : type == "edge" ? TopAbs_EDGE : TopAbs_VERTEX;
}
const TopoDS_Shape& entity(const PartShape& ps, const std::string& type, int index) {
  const auto& map = map_of(ps, type);
  if (index < 1 || index > map.Extent())
    throw Error("not_found", "없는 엔티티입니다: " + type + " " + std::to_string(index), {{"param", "index"}, {"count", map.Extent()}});
  return map(index);
}

Json bbox_json(const TopoDS_Shape& s) {
  Bnd_Box box;
  BRepBndLib::AddOptimal(s, box, false, false);
  if (box.IsVoid()) return Json();
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  return Json{{"min", {x0, y0, z0}}, {"max", {x1, y1, z1}}};
}
Json xyz(const gp_Pnt& p) { return Json::array({p.X(), p.Y(), p.Z()}); }

const char* surface_name(GeomAbs_SurfaceType t) {
  switch (t) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BezierSurface: return "bezier";
    case GeomAbs_BSplineSurface: return "bspline";
    case GeomAbs_SurfaceOfRevolution: return "revolution";
    case GeomAbs_SurfaceOfExtrusion: return "extrusion";
    default: return "other";
  }
}
const char* curve_name(GeomAbs_CurveType t) {
  switch (t) {
    case GeomAbs_Line: return "line";
    case GeomAbs_Circle: return "circle";
    case GeomAbs_Ellipse: return "ellipse";
    case GeomAbs_BezierCurve: return "bezier";
    case GeomAbs_BSplineCurve: return "bspline";
    default: return "other";
  }
}

Json entity_info(const TopoDS_Shape& s, const std::string& type, int index) {
  Json j{{"type", type}, {"index", index}};
  GProp_GProps g;
  if (type == "solid") {
    BRepGProp::VolumeProperties(s, g);
    j["volume"] = g.Mass(), j["center"] = xyz(g.CentreOfMass());
  } else if (type == "face") {
    BRepGProp::SurfaceProperties(s, g);
    j["area"] = g.Mass(), j["center"] = xyz(g.CentreOfMass());
    const BRepAdaptor_Surface surf(TopoDS::Face(s));
    j["surface"] = surface_name(surf.GetType());
    if (surf.GetType() == GeomAbs_Plane) {  // 평면의 법선(면의 방향을 반영: 솔리드 바깥쪽)
      gp_Dir n = surf.Plane().Axis().Direction();
      if (s.Orientation() == TopAbs_REVERSED) n.Reverse();
      j["normal"] = {n.X(), n.Y(), n.Z()};
    }
  } else if (type == "edge") {
    BRepGProp::LinearProperties(s, g);
    j["length"] = g.Mass(), j["center"] = xyz(g.CentreOfMass());
    const TopoDS_Edge e = TopoDS::Edge(s);
    j["curve"] = BRep_Tool::Degenerated(e) ? "degenerated" : curve_name(BRepAdaptor_Curve(e).GetType());
  } else {
    j["point"] = xyz(BRep_Tool::Pnt(TopoDS::Vertex(s)));
    return j;
  }
  j["bbox"] = bbox_json(s);
  return j;
}

std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string lower_ext(const std::string& path) {
  std::string e = fs_path(path).extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}
std::string format_of(const Json& p, const std::string& path) {
  if (has(p, "format")) return p["format"].get<std::string>();
  const std::string e = lower_ext(path);
  if (e == ".step" || e == ".stp") return "step";
  if (e == ".iges" || e == ".igs") return "iges";
  if (e == ".brep" || e == ".brp") return "brep";
  throw Error("unsupported", "파일 형식을 알 수 없습니다(format 으로 지정): " + path, {{"param", "format"}});
}

TopoDS_Shape read_file(const std::string& path, const std::string& format) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(fs_path(path), ec)) throw Error("io_error", "파일이 없습니다: " + path, {{"path", path}});
  TopoDS_Shape s;
  if (format == "brep") {
    std::ifstream f(fs_path(path), std::ios::binary);
    BRep_Builder b;
    BRepTools::Read(s, f, b);
  } else if (format == "step") {
    STEPControl_Reader r;
    if (r.ReadFile(path.c_str()) != IFSelect_RetDone) throw Error("parse_error", "STEP 파일을 읽을 수 없습니다: " + path, {{"path", path}});
    r.TransferRoots();
    s = r.OneShape();
  } else {
    IGESControl_Reader r;
    if (r.ReadFile(path.c_str()) != IFSelect_RetDone) throw Error("parse_error", "IGES 파일을 읽을 수 없습니다: " + path, {{"path", path}});
    r.TransferRoots();
    s = r.OneShape();
  }
  if (s.IsNull()) throw Error("parse_error", "파일에 형상이 없습니다: " + path, {{"path", path}});
  return s;
}

}  // namespace

std::string geometry_entity_name(App& a, Id part, const std::string& type, int index) { return entity_name_of(a, part, type, index); }
int geometry_entity_index(App& a, Id part, const std::string& type, const std::string& name) { return entity_index_of(a, part, type, name); }

const TopoDS_Shape& geometry_shape(App& a, Id part) {
  const Object& p = a.model().get(part);
  if (p.kind != "part") throw Error("wrong_kind", "파트가 아닙니다", {{"object", part}, {"expected", "part"}});
  return solid_shape(a, p).shape;
}

std::string geometry_digest(App& a, Id part) {
  std::set<Id> stack;
  return std::to_string(std::hash<std::string>{}(digest_of(a, part, stack)));
}

Json geometry_counts(App& a, Id part) {
  const PartShape& ps = shape_of(a, part);
  return Json{{"solids", ps.solids.Extent()}, {"faces", ps.faces.Extent()}, {"edges", ps.edges.Extent()}, {"vertices", ps.vertices.Extent()}};
}

std::vector<double> geometry_vertices(App& a, Id part) {
  const PartShape& ps = shape_of(a, part);
  std::vector<double> out;
  for (int i = 1; i <= ps.vertices.Extent(); ++i) {
    const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(ps.vertices(i)));
    out.insert(out.end(), {q.X(), q.Y(), q.Z()});
  }
  return out;
}

std::vector<int> geometry_nearest_edges(App& a, Id part, const std::vector<double>& points) {
  const PartShape& ps = shape_of(a, part);
  std::vector<int> out;
  for (std::size_t i = 0; i + 2 < points.size(); i += 3) {
    const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(gp_Pnt(points[i], points[i + 1], points[i + 2]));
    int best = 0;
    double best_d = 1e300;
    for (int e = 1; e <= ps.edges.Extent(); ++e) {
      if (BRep_Tool::Degenerated(TopoDS::Edge(ps.edges(e)))) continue;
      BRepExtrema_DistShapeShape dist(v, ps.edges(e));
      if (dist.IsDone() && dist.Value() < best_d) best_d = dist.Value(), best = e;
    }
    out.push_back(best);
  }
  return out;
}

// --- 육면체 위상(매핑 메싱용)
BlockTopology geometry_block_topology(App& a, Id part, int solid) {
  const PartShape& ps = shape_of(a, part);
  BlockTopology t;
  if (solid < 1 || solid > ps.solids.Extent()) {
    t.reason = "없는 솔리드입니다";
    return t;
  }
  const TopoDS_Shape& s = ps.solids(solid);
  ShapeMap faces, edges, vertices;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  TopExp::MapShapes(s, TopAbs_VERTEX, vertices);
  if (faces.Extent() != 6 || edges.Extent() != 12 || vertices.Extent() != 8) {
    t.reason = "육면체 위상이 아닙니다(면 " + std::to_string(faces.Extent()) + ", 모서리 " + std::to_string(edges.Extent()) + ", 꼭짓점 " +
               std::to_string(vertices.Extent()) + ")";
    return t;
  }
  // 면마다 꼭짓점을 외곽 와이어 순서로(각 면은 모서리 4개여야 한다)
  std::array<std::vector<int>, 6> face_corners;  // 파트 꼭짓점 번호
  for (int f = 1; f <= 6; ++f) {
    const TopoDS_Face face = TopoDS::Face(faces(f));
    const TopoDS_Wire wire = BRepTools::OuterWire(face);
    std::vector<int> ring;
    for (BRepTools_WireExplorer we(wire, face); we.More(); we.Next()) {
      if (BRep_Tool::Degenerated(we.Current())) continue;
      const int v = ps.vertices.FindIndex(we.CurrentVertex());
      if (ring.empty() || ring.back() != v) ring.push_back(v);
    }
    if (!ring.empty() && ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
    if (ring.size() != 4) {
      t.reason = "면 " + std::to_string(ps.faces.FindIndex(face)) + " 의 모서리가 4개가 아닙니다";
      return t;
    }
    face_corners[static_cast<std::size_t>(f - 1)] = ring;
  }
  // 아랫면 = 면 1. 윗면 = 아랫면과 꼭짓점을 하나도 안 나누는 면.
  const std::vector<int>& bottom = face_corners[0];
  int top = -1;
  for (int f = 1; f < 6; ++f) {
    bool shares = false;
    for (int v : face_corners[static_cast<std::size_t>(f)])
      if (std::find(bottom.begin(), bottom.end(), v) != bottom.end()) shares = true;
    if (!shares) top = f;
  }
  if (top < 0) {
    t.reason = "아랫면의 맞은편 면을 찾지 못했습니다";
    return t;
  }
  // 세로 모서리: 아랫면 꼭짓점 → 윗면 꼭짓점
  auto edge_ends = [&](int e) {
    TopoDS_Vertex v1, v2;
    TopExp::Vertices(TopoDS::Edge(edges(e)), v1, v2);
    return std::pair{ps.vertices.FindIndex(v1), ps.vertices.FindIndex(v2)};
  };
  std::array<int, 8> corners{};
  for (int i = 0; i < 4; ++i) corners[static_cast<std::size_t>(i)] = bottom[static_cast<std::size_t>(i)];
  for (int i = 0; i < 4; ++i) {
    int up = 0;
    for (int e = 1; e <= 12 && !up; ++e) {
      const auto [v1, v2] = edge_ends(e);
      const bool in_bottom1 = std::find(bottom.begin(), bottom.end(), v1) != bottom.end();
      const bool in_bottom2 = std::find(bottom.begin(), bottom.end(), v2) != bottom.end();
      if (v1 == bottom[static_cast<std::size_t>(i)] && !in_bottom2) up = v2;
      else if (v2 == bottom[static_cast<std::size_t>(i)] && !in_bottom1) up = v1;
    }
    if (!up) {
      t.reason = "꼭짓점의 세로 모서리를 찾지 못했습니다";
      return t;
    }
    corners[static_cast<std::size_t>(4 + i)] = up;
  }
  // 오른손 좌표계가 되게(아랫면 1→2 × 1→4 가 윗면 쪽을 향해야 한다)
  auto pnt = [&](int v) { return BRep_Tool::Pnt(TopoDS::Vertex(ps.vertices(v))); };
  const gp_Vec e1(pnt(corners[0]), pnt(corners[1])), e3(pnt(corners[0]), pnt(corners[3])), up(pnt(corners[0]), pnt(corners[4]));
  if (e1.Crossed(e3).Dot(up) < 0) std::swap(corners[1], corners[3]), std::swap(corners[5], corners[7]);
  // 블록 축(i, j, k)이 전역 x, y, z 와 가장 비슷하게 놓이도록 오른손 회전 24가지 가운데 고른다(분할 수 [ni, nj, nk] 의 뜻이 예측 가능하게).
  {
    static const int lattice[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    auto slot_of = [&](int i, int j, int k) {
      for (int s = 0; s < 8; ++s)
        if (lattice[s][0] == i && lattice[s][1] == j && lattice[s][2] == k) return s;
      return 0;
    };
    std::array<int, 8> best = corners;
    double best_score = -1e300;
    const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (const auto& perm : perms)
      for (int flips = 0; flips < 8; ++flips) {
        // 새 좌표 (i', j', k'): 새 축 a 는 옛 축 perm[a], flips 의 비트가 켜지면 뒤집는다. 오른손이어야 한다(행렬식 +1)
        int sign[3];
        for (int a_ = 0; a_ < 3; ++a_) sign[a_] = (flips >> a_) & 1 ? -1 : 1;
        const int parity = (perm[0] == 0 && perm[1] == 1) || (perm[0] == 1 && perm[1] == 2) || (perm[0] == 2 && perm[1] == 0) ? 1 : -1;
        if (parity * sign[0] * sign[1] * sign[2] != 1) continue;
        std::array<int, 8> cand{};
        for (int s = 0; s < 8; ++s) {  // 새 자리 s 의 격자 좌표 → 옛 격자 좌표 → 옛 자리
          int old[3];
          for (int a_ = 0; a_ < 3; ++a_) old[perm[a_]] = sign[a_] > 0 ? lattice[s][a_] : 1 - lattice[s][a_];
          cand[static_cast<std::size_t>(s)] = corners[static_cast<std::size_t>(slot_of(old[0], old[1], old[2]))];
        }
        const gp_Vec di(pnt(cand[0]), pnt(cand[1])), dj(pnt(cand[0]), pnt(cand[3])), dk(pnt(cand[0]), pnt(cand[4]));
        auto unit_dot = [](const gp_Vec& v, double x, double y, double z) { return v.Magnitude() > 0 ? v.Normalized().Dot(gp_Vec(x, y, z)) : 0.0; };
        const double score = unit_dot(di, 1, 0, 0) + unit_dot(dj, 0, 1, 0) + unit_dot(dk, 0, 0, 1);
        if (score > best_score + 1e-12) best_score = score, best = cand;
      }
    corners = best;
  }
  t.corners = corners;
  auto slot = [&](int v) {
    for (int k = 0; k < 8; ++k)
      if (corners[static_cast<std::size_t>(k)] == v) return k;
    return -1;
  };
  for (int e = 1; e <= 12; ++e) {
    const auto [v1, v2] = edge_ends(e);
    t.edges[static_cast<std::size_t>(e - 1)] = {ps.edges.FindIndex(edges(e)), slot(v1), slot(v2)};
  }
  for (int f = 1; f <= 6; ++f) {
    std::array<int, 5> row{ps.faces.FindIndex(faces(f)), 0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) row[static_cast<std::size_t>(k + 1)] = slot(face_corners[static_cast<std::size_t>(f - 1)][static_cast<std::size_t>(k)]);
    t.faces[static_cast<std::size_t>(f - 1)] = row;
  }
  t.ok = true;
  return t;
}

std::vector<double> geometry_edge_points(App& a, Id part, int edge, const std::array<double, 3>& from, int n) {
  const PartShape& ps = shape_of(a, part);
  const TopoDS_Edge e = TopoDS::Edge(entity(ps, "edge", edge));
  BRepAdaptor_Curve curve(e);
  GCPnts_UniformAbscissa ua(curve, n + 1);
  if (!ua.IsDone() || ua.NbPoints() != n + 1) throw Error("geometry_failed", "모서리를 나누지 못했습니다: edge " + std::to_string(edge));
  std::vector<gp_Pnt> pts;
  for (int i = 1; i <= ua.NbPoints(); ++i) pts.push_back(curve.Value(ua.Parameter(i)));
  const gp_Pnt start(from[0], from[1], from[2]);
  if (pts.back().Distance(start) < pts.front().Distance(start)) std::reverse(pts.begin(), pts.end());
  std::vector<double> out;
  for (const gp_Pnt& q : pts) out.insert(out.end(), {q.X(), q.Y(), q.Z()});
  return out;
}

std::vector<double> geometry_edge_points_at(App& a, Id part, int edge, const std::array<double, 3>& from, const std::vector<double>& fractions) {
  const PartShape& ps = shape_of(a, part);
  const TopoDS_Edge e = TopoDS::Edge(entity(ps, "edge", edge));
  BRepAdaptor_Curve curve(e);
  const double total = GCPnts_AbscissaPoint::Length(curve);
  const gp_Pnt start(from[0], from[1], from[2]);
  const bool reversed = curve.Value(curve.LastParameter()).Distance(start) < curve.Value(curve.FirstParameter()).Distance(start);
  std::vector<double> out;
  for (double f : fractions) {
    const double s = std::clamp(reversed ? 1.0 - f : f, 0.0, 1.0) * total;
    GCPnts_AbscissaPoint ap(curve, s, curve.FirstParameter());
    const gp_Pnt q = curve.Value(ap.IsDone() ? ap.Parameter() : curve.FirstParameter() + s / total * (curve.LastParameter() - curve.FirstParameter()));
    out.insert(out.end(), {q.X(), q.Y(), q.Z()});
  }
  return out;
}

void geometry_project_to_face(App& a, Id part, int face, std::vector<double>& xyz) {
  const PartShape& ps = shape_of(a, part);
  const TopoDS_Face f = TopoDS::Face(entity(ps, "face", face));
  BRepAdaptor_Surface surf(f);
  if (surf.GetType() == GeomAbs_Plane) return;  // 평면이면 Coons 보간이 이미 면 위다
  Handle(Geom_Surface) geom = BRep_Tool::Surface(f);
  for (std::size_t i = 0; i + 2 < xyz.size(); i += 3) {
    GeomAPI_ProjectPointOnSurf proj(gp_Pnt(xyz[i], xyz[i + 1], xyz[i + 2]), geom);
    if (proj.NbPoints() < 1) continue;
    const gp_Pnt q = proj.NearestPoint();
    xyz[i] = q.X(), xyz[i + 1] = q.Y(), xyz[i + 2] = q.Z();
  }
}

void geometry_project_to_edge(App& a, Id part, int edge, std::vector<double>& xyz) {
  const PartShape& ps = shape_of(a, part);
  const TopoDS_Edge e = TopoDS::Edge(entity(ps, "edge", edge));
  double u0, u1;
  Handle(Geom_Curve) curve = BRep_Tool::Curve(e, u0, u1);
  if (curve.IsNull()) return;
  for (std::size_t i = 0; i + 2 < xyz.size(); i += 3) {
    GeomAPI_ProjectPointOnCurve proj(gp_Pnt(xyz[i], xyz[i + 1], xyz[i + 2]), curve, u0, u1);
    if (proj.NbPoints() < 1) continue;
    const gp_Pnt q = proj.NearestPoint();
    xyz[i] = q.X(), xyz[i + 1] = q.Y(), xyz[i + 2] = q.Z();
  }
}

Tessellation geometry_tessellation(App& a, Id part, double deflection, double angle_deg) {
  const Object& p = a.model().get(part);
  if (p.kind != "part") throw Error("wrong_kind", "파트가 아닙니다", {{"object", part}, {"expected", "part"}});
  const PartShape& ps = solid_shape(a, p);
  if (deflection <= 0) {  // 기본: 경계 상자 대각선의 1/1000
    Bnd_Box box;
    BRepBndLib::Add(ps.shape, box);
    deflection = box.IsVoid() ? 0.1 : std::sqrt(box.SquareExtent()) * 1e-3;
  }
  if (angle_deg <= 0) angle_deg = 20.0;
  // 삼각화는 형상에 붙어 저장된다(사본을 만들지 않으려고 const 를 벗긴다 — 위상은 바뀌지 않는다).
  // 정밀도가 바뀌면 지우고 다시 만든다(이미 있는 더 촘촘한 삼각화는 그대로 쓰이기 때문).
  static std::map<Id, std::pair<double, double>> last_precision;
  if (auto it = last_precision.find(part); it != last_precision.end() && it->second != std::make_pair(deflection, angle_deg)) BRepTools::Clean(ps.shape);
  last_precision[part] = {deflection, angle_deg};
  BRepMesh_IncrementalMesh mesher(ps.shape, deflection, false, angle_deg * kPi / 180.0, true);
  Tessellation t;
  for (int i = 1; i <= ps.faces.Extent(); ++i) {
    const TopoDS_Face face = TopoDS::Face(ps.faces(i));
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const gp_Trsf trsf = loc.Transformation();
    const std::int64_t base = static_cast<std::int64_t>(t.points.size() / 3);
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    BRepAdaptor_Surface surf(face, false);
    for (int n = 1; n <= tri->NbNodes(); ++n) {
      const gp_Pnt q = tri->Node(n).Transformed(trsf);
      t.points.insert(t.points.end(), {q.X(), q.Y(), q.Z()});
      gp_Vec normal(0, 0, 0);
      if (tri->HasUVNodes()) {
        const gp_Pnt2d uv = tri->UVNode(n);
        gp_Pnt pp;
        gp_Vec du, dv;
        surf.D1(uv.X(), uv.Y(), pp, du, dv);
        normal = du.Crossed(dv);
        if (normal.Magnitude() > 1e-30) normal.Normalize();
        normal.Transform(trsf);
        if (reversed) normal.Reverse();
      }
      t.normals.insert(t.normals.end(), {normal.X(), normal.Y(), normal.Z()});
    }
    for (int k = 1; k <= tri->NbTriangles(); ++k) {
      int n1, n2, n3;
      tri->Triangle(k).Get(n1, n2, n3);
      if (reversed) std::swap(n2, n3);
      t.triangles.insert(t.triangles.end(), {base + n1 - 1, base + n2 - 1, base + n3 - 1});
      t.triangle_face.push_back(i);
    }
  }
  t.edge_offsets.push_back(0);
  for (int i = 1; i <= ps.edges.Extent(); ++i) {
    const TopoDS_Edge e = TopoDS::Edge(ps.edges(i));
    if (!BRep_Tool::Degenerated(e)) {
      BRepAdaptor_Curve curve(e);
      GCPnts_TangentialDeflection pts(curve, angle_deg * kPi / 180.0, deflection);
      for (int k = 1; k <= pts.NbPoints(); ++k) {
        const gp_Pnt q = pts.Value(k);
        t.edge_points.insert(t.edge_points.end(), {q.X(), q.Y(), q.Z()});
      }
    }
    t.edge_offsets.push_back(static_cast<std::int64_t>(t.edge_points.size() / 3));
  }
  return t;
}

void register_geometry_commands(App& app) {
  // 형상 커널이 표준 출력에 쓰는 진행 메시지를 끈다(오류는 명령의 결과로 알린다).
  Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
  {
    CommandSpec c = base("geometry.import", 'J', "part", "STEP·IGES·BREP 파일을 가져와 파트를 만든다(형상은 이력의 시작 피처가 된다)", "GEO-01, GEO-44, CMN-13");
    c.undoable = true;
    c.params = {F("path", "string", "파일 경로").call_req().ex("bracket.step"),
                F("format", "string", "형식(없으면 확장자로 정한다)").one_of({"step", "iges", "brep"}),
                F("name", "string", "파트 이름(없으면 파일 이름)"), F("scale", "number", "배율(원본 단위 → 모델 단위)").gt(0),
                F("part", "ref", "이 파트에 더한다(없으면 새 파트)").ref("part"),
                F("assembly", "bool", "STEP 어셈블리를 계층(상위 파트 + 하위 파트)·이름·색 그대로 가져온다(기본 켬). 끄면 한 파트로 합친다"),
                F("unit_system", "string", "원본 파일의 단위계(모델 단위계로 길이를 환산한다. scale 과 함께 주면 둘 다 곱한다)")
                    .one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("in-lbf-s")};
    c.fn = [](App& a, const Json& pin) {
      Json p = pin;
      if (has(p, "unit_system")) {  // 단위계 → 배율
        check_value(a.commands().at("geometry.import").params[6], p["unit_system"], nullptr);
        const Object* st = a.find_settings();
        const std::string model = st ? st->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
        const double k = unit_factor(p["unit_system"].get<std::string>(), model, "length");
        p["scale"] = (has(p, "scale") ? p["scale"].get<double>() : 1.0) * k;
        p.erase("unit_system");
      }
      const std::string path = p["path"].get<std::string>();
      for (std::size_t i : {std::size_t(1), std::size_t(3)})
        if (has(p, a.commands().at("geometry.import").params[i].name.c_str()))
          check_value(a.commands().at("geometry.import").params[i], p[a.commands().at("geometry.import").params[i].name], nullptr);
      const std::string format = format_of(p, path);
      TopoDS_Shape shape;
      try {
        shape = read_file(path, format);
      } catch (const Standard_Failure& e) {
        throw Error("parse_error", std::string("형상 파일을 읽다가 실패했습니다: ") + e.what(), {{"path", path}});
      }
      Id part = has(p, "part") ? part_of(a, Json{{"id", p["part"]}}).id : 0;
      // 파트 이름: 같은 이름이 있으면 번호를 붙인다
      auto unique_name = [&](std::string stem) {
        std::set<std::string> used;
        for (const Object* o : a.model().by_kind("part")) used.insert(o->name);
        if (stem.empty()) stem = "part";
        std::string name = stem;
        for (int n = 2; used.count(name); ++n) name = stem + "-" + std::to_string(n);
        return name;
      };
      const std::u8string stem8 = fs_path(path).stem().u8string();
      const std::string stem(stem8.begin(), stem8.end());
      // STEP 어셈블리(GEO-03): XDE 로 읽어 계층·이름·색을 파트로 옮긴다. 어셈블리가 아니면(단일 형상) 아래의 단일 파트 경로로
      if (format == "step" && !part && p.value("assembly", true)) {
        try {
          Handle(TDocStd_Document) doc;
          XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
          STEPCAFControl_Reader reader;
          reader.SetNameMode(true), reader.SetColorMode(true);
          if (reader.ReadFile(path.c_str()) == IFSelect_RetDone && reader.Transfer(doc)) {
            Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
            TDF_LabelSequence roots;
            st->GetFreeShapes(roots);
            // 잎(형상이 있는 라벨)이 둘 이상일 때만 어셈블리로 본다. 배치만 든 단일 형상은 한 파트로 읽는다
            std::function<int(const TDF_Label&)> leaves = [&](const TDF_Label& l) -> int {
              if (!st->IsAssembly(l)) return 1;
              TDF_LabelSequence comps;
              st->GetComponents(l, comps);
              int n = 0;
              for (int k = 1; k <= comps.Length(); ++k) {
                TDF_Label sub;
                st->GetReferredShape(comps.Value(k), sub);
                n += leaves(sub.IsNull() ? comps.Value(k) : sub);
              }
              return n;
            };
            int leaf_count = 0;
            for (int i = 1; i <= roots.Length(); ++i) leaf_count += leaves(roots.Value(i));
            if (leaf_count > 1) {
              auto label_name = [&](const TDF_Label& l) {
                Handle(TDataStd_Name) nm;
                if (l.FindAttribute(TDataStd_Name::GetID(), nm)) {
                  const TCollection_ExtendedString& es = nm->Get();
                  std::string out;
                  for (int k = 1; k <= es.Length(); ++k) {
                    const unsigned ch = es.Value(k);
                    if (ch < 0x80) out.push_back(static_cast<char>(ch));
                    else if (ch < 0x800) out.push_back(static_cast<char>(0xC0 | (ch >> 6))), out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                    else out.push_back(static_cast<char>(0xE0 | (ch >> 12))), out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F))), out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                  }
                  return out;
                }
                return std::string();
              };
              auto color_of = [&](const TDF_Label& l, Json& out) {
                Quantity_Color c;
                for (XCAFDoc_ColorType t : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen, XCAFDoc_ColorCurv})
                  if (ct->GetColor(l, t, c)) {
                    out = Json::array({c.Red(), c.Green(), c.Blue()});
                    return;
                  }
              };
              Json created = Json::array();
              std::function<void(const TDF_Label&, const TDF_Label&, Id, const TopLoc_Location&)> visit =
                  [&](const TDF_Label& comp, const TDF_Label& ref, Id parent, const TopLoc_Location& loc) {
                    std::string name = label_name(comp);
                    if (name.empty()) name = label_name(ref);
                    Json q{{"name", unique_name(name.empty() ? stem : name)}};
                    if (parent) q["assembly"] = parent;
                    Json color;
                    color_of(comp, color);
                    if (color.is_null()) color_of(ref, color);
                    if (!color.is_null()) q["color"] = color;
                    const Id id = a.invoke("part.create", q)["id"].get<Id>();
                    created.push_back(id);
                    if (st->IsAssembly(ref)) {
                      TDF_LabelSequence comps;
                      st->GetComponents(ref, comps);
                      for (int k = 1; k <= comps.Length(); ++k) {
                        TDF_Label sub;
                        st->GetReferredShape(comps.Value(k), sub);
                        visit(comps.Value(k), sub.IsNull() ? comps.Value(k) : sub, id, loc * st->GetLocation(comps.Value(k)));
                      }
                    } else {
                      TopoDS_Shape sh = st->GetShape(ref);
                      if (!loc.IsIdentity()) sh = sh.Moved(loc);
                      Json fq{{"parent", id}, {"brep", write_brep(sh)}, {"source", path}, {"format", format}};
                      if (has(p, "scale")) fq["scale"] = p["scale"];
                      a.invoke("feature.create_import", fq);
                    }
                  };
              for (int i = 1; i <= roots.Length(); ++i) visit(roots.Value(i), roots.Value(i), 0, TopLoc_Location());
              return Json{{"id", created.empty() ? Json() : created[0]}, {"parts", created}, {"format", format}, {"assembly", true}};
            }
          }
        } catch (const Standard_Failure& e) {
          throw Error("parse_error", std::string("STEP 어셈블리를 읽다가 실패했습니다: ") + e.what(), {{"path", path}});
        }
      }
      if (!part) {
        Json q = Json::object();
        q["name"] = has(p, "name") ? p["name"].get<std::string>() : unique_name(stem);
        part = a.invoke("part.create", q)["id"].get<Id>();
      }
      Json fq{{"parent", part}, {"brep", write_brep(shape)}, {"source", path}, {"format", format}};
      if (has(p, "scale")) fq["scale"] = p["scale"];
      const Id feature = a.invoke("feature.create_import", fq)["id"].get<Id>();
      // 단일 형상의 색(XDE 가 읽은 것)
      if (format == "step") {
        try {
          Handle(TDocStd_Document) doc;
          XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
          STEPCAFControl_Reader reader;
          reader.SetColorMode(true);
          if (reader.ReadFile(path.c_str()) == IFSelect_RetDone && reader.Transfer(doc)) {
            Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
            TDF_LabelSequence roots;
            st->GetFreeShapes(roots);
            // 색: 뿌리 라벨 → 그 구성 요소·참조 라벨 순으로 찾는다
            Quantity_Color c;
            bool found = false;
            std::function<void(const TDF_Label&)> find_color = [&](const TDF_Label& l) {
              if (found) return;
              if (ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c)) {
                found = true;
                return;
              }
              if (st->IsAssembly(l)) {
                TDF_LabelSequence comps;
                st->GetComponents(l, comps);
                for (int k = 1; k <= comps.Length() && !found; ++k) {
                  find_color(comps.Value(k));
                  TDF_Label sub;
                  st->GetReferredShape(comps.Value(k), sub);
                  if (!sub.IsNull()) find_color(sub);
                }
              }
            };
            for (int i = 1; i <= roots.Length() && !found; ++i) find_color(roots.Value(i));
            if (found) a.invoke("part.update", Json{{"id", part}, {"color", Json::array({c.Red(), c.Green(), c.Blue()})}});
          }
        } catch (const Standard_Failure&) {
        }
      }
      const PartShape& ps = shape_of(a, part);
      return Json{{"id", part}, {"feature", feature}, {"format", format}, {"solids", ps.solids.Extent()}, {"faces", ps.faces.Extent()},
                  {"edges", ps.edges.Extent()}, {"vertices", ps.vertices.Extent()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.export", 'J', "part", "파트의 형상을 STEP·BREP 으로 내보낸다", "GEO-02");
    c.params = {F("id", "ref", "파트").call_req(), F("path", "string", "파일 경로").call_req().ex("part.step"),
                F("format", "string", "형식(없으면 확장자로 정한다)").one_of({"step", "brep"})};
    c.fn = [](App& a, const Json& p) {
      const Object& root = part_of(a, p);
      const std::string path = p["path"].get<std::string>();
      const std::string format = format_of(p, path);
      auto children_of = [&](Id id) {
        std::vector<const Object*> out;
        for (const Object* o : a.model().by_kind("part"))
          if (!o->suppressed && o->props.contains("assembly") && o->props["assembly"].is_number() && o->props["assembly"].get<Id>() == id) out.push_back(o);
        return out;
      };
      if (format == "brep") {
        const PartShape& ps = solid_shape(a, root);
        std::ofstream f(fs_path(path), std::ios::binary);
        if (!f) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
        BRepTools::Write(ps.shape, f);
      } else if (format == "step") {
        // XDE 문서로 쓴다: 하위 파트(assembly 참조)가 있으면 어셈블리 계층, 파트 이름, 색이 함께 나간다(GEO-03)
        try {
          Handle(TDocStd_Document) doc;
          XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
          Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
          Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
          auto set_name = [&](const TDF_Label& l, const std::string& name) { TDataStd_Name::Set(l, TCollection_ExtendedString(name.c_str(), true)); };
          auto set_color = [&](const TDF_Label& l, const Object& o) {
            if (o.props.contains("color") && o.props["color"].is_array() && o.props["color"].size() == 3) {
              const Json& c = o.props["color"];
              ct->SetColor(l, Quantity_Color(c[0].get<double>(), c[1].get<double>(), c[2].get<double>(), Quantity_TOC_RGB), XCAFDoc_ColorSurf);
              ct->SetColor(l, Quantity_Color(c[0].get<double>(), c[1].get<double>(), c[2].get<double>(), Quantity_TOC_RGB), XCAFDoc_ColorGen);
            }
          };
          std::function<TDF_Label(const Object&)> add = [&](const Object& o) -> TDF_Label {
            const std::vector<const Object*> kids = children_of(o.id);
            TDF_Label l;
            if (kids.empty()) {
              l = st->AddShape(solid_shape(a, o).shape, false);
            } else {
              l = st->NewShape();
              for (const Object* k : kids) {
                const TDF_Label kl = add(*k);
                const TDF_Label comp = st->AddComponent(l, kl, TopLoc_Location());
                set_name(comp, k->name);
              }
              st->UpdateAssemblies();
            }
            set_name(l, o.name);
            set_color(l, o);
            return l;
          };
          add(root);
          st->UpdateAssemblies();
          STEPCAFControl_Writer w;
          w.SetNameMode(true), w.SetColorMode(true);
          if (!w.Transfer(doc, STEPControl_AsIs) || w.Write(path.c_str()) != IFSelect_RetDone)
            throw Error("io_error", "STEP 파일을 쓸 수 없습니다: " + path, {{"path", path}});
        } catch (const Standard_Failure& e) {
          throw Error("io_error", std::string("STEP 파일을 쓰다가 실패했습니다: ") + e.what(), {{"path", path}});
        }
      } else {
        throw Error("unsupported", "이 형식으로는 내보내지 못합니다: " + format, {{"param", "format"}});
      }
      return Json{{"path", path}, {"format", format}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.entity_by_name", 'Q', "part", "영속 이름표(geometry.entities 의 name)로 지금 번호를 찾는다. 없으면 index=0 (형상이 바뀌어 사라짐)", "GEO-05");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "종류").call_req().one_of({"solid", "face", "edge", "vertex"}).ex("face"),
                F("name", "string", "이름표").call_req().ex("f2:face:1")};
    c.fn = [](App& a, const Json& p) {
      const Id part = part_of(a, p).id;
      const std::string type = p["type"].get<std::string>(), name = p["name"].get<std::string>();
      const int index = geometry_entity_index(a, part, type, name);
      Json out{{"id", part}, {"type", type}, {"name", name}, {"index", index}};
      if (index) out["info"] = entity_info(entity(shape_of(a, part), type, index), type, index);
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.entities", 'Q', "part", "파트의 solid·face·edge·vertex 개수를 조회한다. type 을 주면 그 종류의 목록(영속 이름표 name 포함)도 준다", "GEO-04, GEO-05");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "목록으로 볼 종류").one_of({"solid", "face", "edge", "vertex"})};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = shape_of(a, part_of(a, p).id);
      Json out{{"solids", ps.solids.Extent()}, {"faces", ps.faces.Extent()}, {"edges", ps.edges.Extent()}, {"vertices", ps.vertices.Extent()},
               {"ok", ps.ok}};
      if (has(p, "type")) {
        const std::string type = p["type"].get<std::string>();
        const auto& map = map_of(ps, type);
        Json list = Json::array();
        for (int i = 1; i <= map.Extent(); ++i) {
          Json info = entity_info(map(i), type, i);
          info["name"] = geometry_entity_name(a, part_of(a, p).id, type, i);  // 영속 이름표(D9)
          list.push_back(std::move(info));
        }
        out["entities"] = list;
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.entity_info", 'Q', "part", "엔티티의 종류·곡면(곡선) 종류·크기·경계 상자를 조회한다", "GEO-04");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "엔티티 종류").call_req().one_of({"solid", "face", "edge", "vertex"}).ex("face"),
                F("index", "integer", "엔티티 번호(1 부터)").call_req().ge(1).ex(1)};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = solid_shape(a, part_of(a, p));
      const std::string type = p["type"].get<std::string>();
      Json info = entity_info(entity(ps, type, p["index"].get<int>()), type, p["index"].get<int>());
      info["name"] = geometry_entity_name(a, part_of(a, p).id, type, p["index"].get<int>());
      return info;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.find", 'Q', "part",
                         "조건으로 엔티티를 찾는다: 종류, 곡면·곡선 종류, 상자 안(중심 기준), 크기 범위(면적·길이·체적), 법선 방향(평면), 접선 연속 이웃(seed)", "GEO-07");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "엔티티 종류").call_req().one_of({"solid", "face", "edge", "vertex"}).ex("face"),
                F("surface", "string", "곡면(곡선) 종류(예: plane, cylinder, line, circle)").ex("plane"),
                F("box_min", "vector3", "중심이 이 상자 안인 것(최소 모서리)").unit("length"), F("box_max", "vector3", "상자의 최대 모서리").unit("length"),
                F("min_size", "number", "크기(면적·길이·체적)의 하한").ge(0), F("max_size", "number", "크기의 상한").ge(0),
                F("normal", "vector3", "평면의 법선이 이 방향과 나란한 면만(각도 tolerance)"), F("tolerance", "number", "법선 각 허용(도, 기본 1)").gt(0),
                F("seed", "integer", "이 엔티티와 접선 연속으로 이어진 것만(면은 모서리를 공유하며 법선이 이어진 면)").ge(1)};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = solid_shape(a, part_of(a, p));
      const std::string type = p["type"].get<std::string>();
      const auto& map = map_of(ps, type);
      const double tol = p.value("tolerance", 1.0) * kPi / 180.0;
      std::vector<Json> infos;
      for (int i = 1; i <= map.Extent(); ++i) infos.push_back(entity_info(map(i), type, i));
      auto size_of_info = [&](const Json& info) {
        for (const char* k : {"area", "length", "volume"})
          if (info.contains(k)) return info[k].get<double>();
        return 0.0;
      };
      std::vector<int> found;
      for (int i = 1; i <= map.Extent(); ++i) {
        const Json& info = infos[static_cast<std::size_t>(i - 1)];
        if (has(p, "surface") && info.value("surface", info.value("curve", std::string())) != p["surface"].get<std::string>()) continue;
        if (has(p, "box_min") || has(p, "box_max")) {
          const Json c = info.contains("center") ? info["center"] : info.value("point", Json::array({0, 0, 0}));
          bool inside = true;
          for (std::size_t k = 0; k < 3; ++k) {
            if (has(p, "box_min") && c[k].get<double>() < p["box_min"][k].get<double>() - 1e-9) inside = false;
            if (has(p, "box_max") && c[k].get<double>() > p["box_max"][k].get<double>() + 1e-9) inside = false;
          }
          if (!inside) continue;
        }
        const double size = size_of_info(info);
        if (has(p, "min_size") && size < p["min_size"].get<double>()) continue;
        if (has(p, "max_size") && size > p["max_size"].get<double>()) continue;
        if (has(p, "normal")) {
          if (type != "face" || !info.contains("normal")) continue;
          const gp_Dir n(info["normal"][0].get<double>(), info["normal"][1].get<double>(), info["normal"][2].get<double>());
          if (n.Angle(dir(p, "normal", true)) > tol) continue;
        }
        found.push_back(i);
      }
      if (has(p, "seed")) {  // 접선 연속: seed 에서 모서리를 공유하는 면 가운데 그 모서리에서 법선이 이어진 면으로 퍼진다(면만)
        if (type != "face") throw Error("invalid_param", "seed 는 면에서만 쓴다", {{"param", "seed"}});
        const int seed = p["seed"].get<int>();
        if (seed < 1 || seed > map.Extent()) throw Error("not_found", "없는 면입니다: " + std::to_string(seed), {{"param", "seed"}});
        AncestorMap anc;
        TopExp::MapShapesAndAncestors(ps.shape, TopAbs_EDGE, TopAbs_FACE, anc);
        std::set<int> region{seed};
        std::vector<int> stack{seed};
        while (!stack.empty()) {
          const int f = stack.back();
          stack.pop_back();
          for (TopExp_Explorer ex(map(f), TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopoDS_Edge e = TopoDS::Edge(ex.Current());
            if (BRep_Tool::Degenerated(e) || !anc.Contains(e)) continue;
            for (const TopoDS_Shape& other : anc.FindFromKey(e)) {
              const int g = map.FindIndex(other);
              if (!g || region.count(g)) continue;
              // 모서리 가운데 점에서 두 면의 법선을 비교한다
              BRepAdaptor_Curve curve(e);
              const gp_Pnt mid = curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
              auto normal_at = [&](const TopoDS_Face& face) {
                GeomAPI_ProjectPointOnSurf proj(mid, BRep_Tool::Surface(face));
                double u, v;
                proj.LowerDistanceParameters(u, v);
                BRepLProp_SLProps props(BRepAdaptor_Surface(face), u, v, 1, 1e-9);
                gp_Dir n = props.IsNormalDefined() ? props.Normal() : gp_Dir(0, 0, 1);
                if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
                return n;
              };
              if (normal_at(TopoDS::Face(map(f))).Angle(normal_at(TopoDS::Face(other))) <= tol) region.insert(g), stack.push_back(g);
            }
          }
        }
        std::vector<int> kept;
        for (int i : found)
          if (region.count(i)) kept.push_back(i);
        found = kept;
      }
      return Json{{"type", type}, {"indices", found}, {"count", found.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.adjacency", 'Q', "part", "엔티티에 이웃한 엔티티를 조회한다(예: 모서리를 공유하는 면, 면의 모서리)", "GEO-04");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "엔티티 종류").call_req().one_of({"solid", "face", "edge", "vertex"}).ex("face"),
                F("index", "integer", "엔티티 번호").call_req().ge(1).ex(1),
                F("to", "string", "찾을 종류(없으면 같은 종류의 이웃)").one_of({"solid", "face", "edge", "vertex"})};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = solid_shape(a, part_of(a, p));
      const std::string type = p["type"].get<std::string>(), to = p.value("to", type);
      const TopoDS_Shape& s = entity(ps, type, p["index"].get<int>());
      const auto& target = map_of(ps, to);
      std::set<int> found;
      auto down = [&](const TopoDS_Shape& from, const std::string& kind, std::set<int>& out) {
        for (TopExp_Explorer ex(from, enum_of(kind)); ex.More(); ex.Next()) out.insert(map_of(ps, kind).FindIndex(ex.Current()));
      };
      auto up = [&](const TopoDS_Shape& from, const std::string& from_kind, const std::string& kind, std::set<int>& out) {
        AncestorMap anc;
        TopExp::MapShapesAndAncestors(ps.shape, enum_of(from_kind), enum_of(kind), anc);
        if (!anc.Contains(from)) return;
        for (const TopoDS_Shape& x : anc.FindFromKey(from)) out.insert(map_of(ps, kind).FindIndex(x));
      };
      static const std::map<std::string, int> rank = {{"vertex", 0}, {"edge", 1}, {"face", 2}, {"solid", 3}};
      if (rank.at(to) < rank.at(type)) {
        down(s, to, found);
      } else if (rank.at(to) > rank.at(type)) {
        up(s, type, to, found);
      } else {  // 같은 종류: 한 단계 아래를 공유하는 것(면 → 모서리를 공유하는 면). 꼭짓점은 모서리로 이어진 꼭짓점
        const std::string link = type == "vertex" ? "edge" : type == "edge" ? "vertex" : type == "face" ? "edge" : "face";
        std::set<int> links;
        if (type == "vertex") up(s, type, link, links); else down(s, link, links);
        for (int l : links) {
          if (type == "vertex") down(map_of(ps, link)(l), to, found); else up(map_of(ps, link)(l), link, to, found);
        }
        found.erase(target.FindIndex(s));
      }
      found.erase(0);
      return Json{{"type", to}, {"indices", std::vector<int>(found.begin(), found.end())}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.measure", 'Q', "part", "체적·면적·길이·질량중심·관성·경계 상자를 계산한다(파트 전체 또는 엔티티 하나)", "GEO-08");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "엔티티 종류").one_of({"solid", "face", "edge", "vertex"}),
                F("index", "integer", "엔티티 번호").ge(1)};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = solid_shape(a, part_of(a, p));
      if (has(p, "type")) {
        if (!has(p, "index")) throw Error("missing_param", "필수 매개변수가 없습니다: index", {{"param", "index"}});
        const std::string type = p["type"].get<std::string>();
        Json info = entity_info(entity(ps, type, p["index"].get<int>()), type, p["index"].get<int>());
      info["name"] = geometry_entity_name(a, part_of(a, p).id, type, p["index"].get<int>());
      return info;
      }
      GProp_GProps vol, area;
      if (ps.solids.Extent()) {  // 체적은 솔리드만으로 센다(같은 파트에 남은 모서리·면 바디가 섞이지 않게)
        std::vector<TopoDS_Shape> solids;
        for (int i = 1; i <= ps.solids.Extent(); ++i) solids.push_back(ps.solids(i));
        BRepGProp::VolumeProperties(compound_of(solids), vol);
      }
      BRepGProp::SurfaceProperties(ps.shape, area);
      Json out{{"volume", ps.solids.Extent() ? vol.Mass() : 0.0}, {"area", area.Mass()}, {"bbox", bbox_json(ps.shape)}};
      const GProp_GProps& g = ps.solids.Extent() ? vol : area;
      out["center"] = xyz(g.CentreOfMass());
      const gp_Mat m = g.MatrixOfInertia();  // 질량중심 기준, 밀도 1
      out["inertia"] = Json::array({Json::array({m(1, 1), m(1, 2), m(1, 3)}), Json::array({m(2, 1), m(2, 2), m(2, 3)}),
                                    Json::array({m(3, 1), m(3, 2), m(3, 3)})});
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.check", 'Q', "part", "형상의 유효성, 자유 경계, 비다양체 모서리, 미세 모서리를 검사한다", "GEO-09");
    c.params = {F("id", "ref", "파트").call_req(), F("small_edge", "number", "이 길이보다 짧은 모서리를 미세 모서리로 본다").gt(0).unit("length")};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = solid_shape(a, part_of(a, p));
      AncestorMap faces_of_edge;
      TopExp::MapShapesAndAncestors(ps.shape, TopAbs_EDGE, TopAbs_FACE, faces_of_edge);
      // 비다양체는 솔리드마다 센다: share_topology 로 두 솔리드가 함께 쓰는 경계 면의 모서리는 전체로는 면 3개가 닿지만
      // 솔리드 하나 안에서는 2개다(정상). 솔리드에 들지 않은 모서리(껍질·면 바디)만 전체 개수로 본다
      std::vector<AncestorMap> per_solid;
      for (int s = 1; s <= ps.solids.Extent(); ++s) {
        per_solid.emplace_back();
        TopExp::MapShapesAndAncestors(ps.solids(s), TopAbs_EDGE, TopAbs_FACE, per_solid.back());
      }
      Json free_edges = Json::array(), non_manifold = Json::array(), small = Json::array();
      Bnd_Box box;
      BRepBndLib::Add(ps.shape, box);
      const double limit = has(p, "small_edge") ? p["small_edge"].get<double>() : (box.IsVoid() ? 0.0 : std::sqrt(box.SquareExtent()) * 1e-4);
      for (int i = 1; i <= ps.edges.Extent(); ++i) {
        const TopoDS_Edge e = TopoDS::Edge(ps.edges(i));
        if (BRep_Tool::Degenerated(e)) continue;
        const int n = faces_of_edge.Contains(e) ? faces_of_edge.FindFromKey(e).Extent() : 0;
        // 닫힌 면(원통의 이음선)은 같은 면이 두 번 나온다
        if (n == 1 && !BRep_Tool::IsClosed(e, TopoDS::Face(faces_of_edge.FindFromKey(e).First()))) free_edges.push_back(i);
        int worst = 0;
        bool in_solid = false;
        for (const AncestorMap& m : per_solid)
          if (m.Contains(e)) in_solid = true, worst = std::max(worst, m.FindFromKey(e).Extent());
        if ((in_solid ? worst : n) > 2) non_manifold.push_back(i);
        GProp_GProps g;
        BRepGProp::LinearProperties(e, g);
        if (g.Mass() < limit) small.push_back(i);
      }
      const bool valid = BRepCheck_Analyzer(ps.shape).IsValid();
      return Json{{"valid", valid}, {"free_edges", free_edges}, {"non_manifold_edges", non_manifold}, {"small_edges", small},
                  {"small_edge_limit", limit}, {"ok", valid && free_edges.empty() && non_manifold.empty()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.tessellation", 'Q', "part", "표시용 삼각화의 크기를 조회한다(배열은 Python 의 app.geometry.tessellation)", "GEO-06");
    c.params = {F("id", "ref", "파트").call_req(), F("deflection", "number", "곡면과 삼각형 사이의 허용 거리(없으면 크기의 1/1000)").gt(0).unit("length"),
                F("angle", "number", "이웃 삼각형 사이의 허용 각(도, 기본 20)").gt(0)};
    c.fn = [](App& a, const Json& p) {
      const Tessellation t = geometry_tessellation(a, part_of(a, p).id, p.value("deflection", 0.0), p.value("angle", 0.0));
      return Json{{"points", t.points.size() / 3}, {"triangles", t.triangles.size() / 3}, {"edges", t.edge_offsets.size() - 1},
                  {"edge_points", t.edge_points.size() / 3}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("feature.status", 'Q', "part", "피처마다 적용 상태(ok·error·skipped·suppressed·rolled_back)와 오류를 조회한다", "GEO-42");
    c.params = {F("id", "ref", "파트").call_req()};
    c.fn = [](App& a, const Json& p) {
      const PartShape& ps = shape_of(a, part_of(a, p).id);
      return Json{{"ok", ps.ok}, {"features", ps.status}};
    };
    app.register_command(std::move(c));
  }
  {
    // 이력 시점(GEO-41): 파트의 rollback 속성 = 이 피처까지만 적용. feature 를 비우면 끝까지
    CommandSpec c = base("feature.rollback", 'C', "part", "이력의 지정 피처까지만 적용한다(feature 를 비우면 끝까지 되돌린다). 뒤의 피처는 rolled_back 상태가 된다", "GEO-41");
    c.params = {F("id", "ref", "파트").call_req(), F("feature", "ref", "이 피처까지 적용(없으면 전부)").ref("feature")};
    c.fn = [](App& a, const Json& p) {
      Object part = part_of(a, p);  // 복사(바꿔서 다시 넣는다)
      if (has(p, "feature")) {
        const Object& f = a.model().get(p["feature"].get<Id>());
        if (f.kind != "feature" || f.parent != part.id) throw Error("invalid_param", "이 파트의 피처가 아닙니다", {{"param", "feature"}, {"object", f.id}});
        part.props["rollback"] = f.id;
      } else {
        part.props.erase("rollback");
      }
      a.model().replace(part);
      const PartShape& ps = shape_of(a, part.id);
      return Json{{"id", part.id}, {"rollback", has(p, "feature") ? p["feature"] : Json()}, {"ok", ps.ok}, {"features", ps.status}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("feature.reorder", 'C', "feature", "피처를 같은 파트 안에서 지정 위치(0 부터)로 옮긴다. 뒤 피처가 가리키는 엔티티 번호는 앞 피처까지의 형상 기준이므로 상태를 확인한다", "GEO-41");
    c.params = {F("id", "ref", "피처").call_req().ref("feature"), F("index", "integer", "새 위치(0 부터)").call_req().ge(0)};
    c.fn = [](App& a, const Json& p) {
      const Object& f = a.model().get(p["id"].get<Id>());
      if (f.kind != "feature") throw Error("wrong_kind", "피처가 아닙니다", {{"object", f.id}, {"expected", "feature"}});
      check_value(a.commands().at("feature.reorder").params[1], p["index"], nullptr);
      const Json r = a.invoke("feature.move", Json{{"id", f.id}, {"index", p["index"]}});
      const PartShape& ps = shape_of(a, f.parent);
      return Json{{"id", f.id}, {"index", r["index"]}, {"ok", ps.ok}, {"features", ps.status}};
    };
    app.register_command(std::move(c));
  }
  {
    // 끊어진 참조 다시 잇기(GEO-42): 피처의 엔티티 번호 속성(faces·edges·face 등)의 값을 바꾼다. 새 번호는 앞 피처까지의 형상에 있어야 한다
    CommandSpec c = base("feature.rebind", 'C', "feature",
                         "피처가 가리키는 엔티티 번호를 바꾼다(field 의 from → to, 또는 entities 로 목록 전체). 새 번호는 앞 피처까지의 형상 기준으로 검사한다",
                         "GEO-42");
    c.params = {F("id", "ref", "피처").call_req().ref("feature"), F("field", "string", "엔티티 번호 속성 이름").call_req().ex("edges"),
                F("from", "integer", "바꿀 번호").ge(1), F("to", "integer", "새 번호").ge(1), F("entities", "integer_list", "목록 전체를 이것으로").ge(1)};
    c.fn = [](App& a, const Json& p) {
      Object f = a.model().get(p["id"].get<Id>());
      if (f.kind != "feature") throw Error("wrong_kind", "피처가 아닙니다", {{"object", f.id}, {"expected", "feature"}});
      const std::string field = p["field"].get<std::string>();
      std::optional<FieldSpec> spec;
      for (const FieldSpec& s : a.schema().get("feature").fields_for(f.props.value("type", std::string())))
        if (s.name == field) spec = s;
      if (!spec || (spec->type != "integer_list" && spec->type != "integer"))
        throw Error("invalid_param", "엔티티 번호 속성이 아닙니다: " + field, {{"param", "field"}});
      const std::string type = field == "faces" || field == "face" ? "face" : field == "edges" || field == "edge" ? "edge" : field == "vertices" || field == "vertex" ? "vertex" : field == "solids" ? "solid" : "";
      // 앞 피처까지의 형상에서 번호가 유효한지
      std::vector<TopoDS_Shape> bodies;
      for (const Object* g : a.model().children(f.parent, "feature")) {
        if (g->id == f.id) break;
        if (!g->suppressed) apply(a, *g, bodies);
      }
      ShapeMap map;
      if (!type.empty() && !bodies.empty())
        TopExp::MapShapes(compound_of(bodies), type == "face" ? TopAbs_FACE : type == "edge" ? TopAbs_EDGE : type == "vertex" ? TopAbs_VERTEX : TopAbs_SOLID, map);
      auto check = [&](int n) {
        if (!type.empty() && (n < 1 || n > map.Extent()))
          throw Error("out_of_range", "앞 피처까지의 형상에 없는 " + type + " 번호입니다: " + std::to_string(n), {{"param", has(p, "entities") ? "entities" : "to"}, {"count", map.Extent()}});
      };
      Json value = f.props.value(field, Json());
      if (has(p, "entities")) {
        for (const Json& n : p["entities"]) check(n.get<int>());
        value = spec->type == "integer" ? p["entities"][0] : p["entities"];
      } else {
        if (!has(p, "from") || !has(p, "to")) throw Error("missing_param", "from·to 또는 entities 가 필요합니다", {{"param", "to"}});
        check(p["to"].get<int>());
        bool found = false;
        if (value.is_array()) {
          for (Json& n : value)
            if (n == p["from"]) n = p["to"], found = true;
        } else if (value == p["from"]) {
          value = p["to"], found = true;
        }
        if (!found) throw Error("not_found", "피처가 그 번호를 가리키지 않습니다", {{"param", "from"}, {"value", value}});
      }
      f.props[field] = value;
      a.model().replace(f);
      const PartShape& ps = shape_of(a, f.parent);
      Json mine;
      for (const Json& st : ps.status)
        if (st["feature"] == f.id) mine = st;
      return Json{{"id", f.id}, {"field", field}, {"value", value}, {"status", mine}, {"ok", ps.ok}};
    };
    app.register_command(std::move(c));
  }
  {
    // 엔티티 이력(GEO-20): 피처를 차례로 적용하며 면·모서리·꼭짓점·솔리드가 어느 피처에서 생기고 사라졌는지 본다(형상 동일성 IsSame 기준).
    // type·index 를 주면 현재 형상의 그 엔티티가 처음 나타난 피처와 거쳐 온 피처를 돌려준다
    CommandSpec c = base("feature.history", 'Q', "part",
                         "피처마다 생기고 사라진 엔티티 수를, type·index 를 주면 현재 형상의 그 엔티티를 만든 피처와 거쳐 온 피처를 조회한다", "GEO-20");
    c.params = {F("id", "ref", "파트").call_req(), F("type", "string", "엔티티 종류").one_of({"solid", "face", "edge", "vertex"}),
                F("index", "integer", "현재 형상의 엔티티 번호").ge(1)};
    c.fn = [](App& a, const Json& p) {
      const Object& part = part_of(a, p);
      const Id rollback = has(part.props, "rollback") ? part.props["rollback"].get<Id>() : 0;
      static const std::vector<std::pair<std::string, TopAbs_ShapeEnum>> kinds = {
          {"solid", TopAbs_SOLID}, {"face", TopAbs_FACE}, {"edge", TopAbs_EDGE}, {"vertex", TopAbs_VERTEX}};
      std::vector<TopoDS_Shape> bodies;
      std::map<std::string, ShapeMap> prev;
      struct Step {
        Id feature;
        std::string name, state;
        std::map<std::string, ShapeMap> maps;
      };
      std::vector<Step> steps;
      bool failed = false, past = false;
      for (const Object* f : a.model().children(part.id, "feature")) {
        Step st{f->id, f->name, "ok", {}};
        if (past) st.state = "rolled_back";
        else if (f->suppressed) st.state = "suppressed";
        else if (failed) st.state = "skipped";
        else {
          try {
            apply(a, *f, bodies);
          } catch (const Error&) {
            st.state = "error", failed = true;
          } catch (const Standard_Failure&) {
            st.state = "error", failed = true;
          }
        }
        if (!bodies.empty()) {
          const TopoDS_Shape shape = compound_of(bodies);
          for (const auto& [name, kind] : kinds) TopExp::MapShapes(shape, kind, st.maps[name]);
        }
        steps.push_back(std::move(st));
        if (rollback && f->id == rollback) past = true;
      }
      // 같은 엔티티로 보는 기준: 형상 동일성(IsSame) 또는 같은 종류·같은 크기(넓이·길이·부피)·같은 무게중심.
      // 필렛처럼 형상 전체를 다시 만드는 연산은 손대지 않은 면도 새 TShape 가 되므로 기하로도 본다
      struct Sig {
        gp_Pnt center;
        double measure;
      };
      auto sig_of = [](const TopoDS_Shape& s) {
        GProp_GProps g;
        switch (s.ShapeType()) {
          case TopAbs_VERTEX: return Sig{BRep_Tool::Pnt(TopoDS::Vertex(s)), 0.0};
          case TopAbs_EDGE: BRepGProp::LinearProperties(s, g); break;
          case TopAbs_FACE: BRepGProp::SurfaceProperties(s, g); break;
          default: BRepGProp::VolumeProperties(s, g); break;
        }
        return Sig{g.CentreOfMass(), g.Mass()};
      };
      std::map<const void*, Sig> sig_cache;
      auto sig = [&](const TopoDS_Shape& s) -> const Sig& {
        const void* key = s.TShape().get();
        auto it = sig_cache.find(key);
        if (it == sig_cache.end()) it = sig_cache.emplace(key, sig_of(s)).first;
        return it->second;
      };
      auto contains = [&](const ShapeMap& map, const TopoDS_Shape& s) {
        for (int i = 1; i <= map.Extent(); ++i)
          if (map(i).IsSame(s)) return true;
        const Sig& a_ = sig(s);
        const double scale = std::max(1.0, std::fabs(a_.measure));
        for (int i = 1; i <= map.Extent(); ++i) {
          const Sig& b_ = sig(map(i));
          if (std::fabs(a_.measure - b_.measure) <= 1e-9 * scale && a_.center.Distance(b_.center) <= 1e-7 * std::max(1.0, std::cbrt(scale))) return true;
        }
        return false;
      };
      if (has(p, "type") || has(p, "index")) {
        if (!has(p, "type") || !has(p, "index")) throw Error("missing_param", "type 과 index 를 함께 주세요", {{"param", "index"}});
        const std::string type = p["type"].get<std::string>();
        check_value(a.commands().at("feature.history").params[1], p["type"], nullptr);
        // 현재 형상의 번호는 마지막 단계의 맵 번호와 같다(같은 연산을 같은 순서로 되풀이하므로). 캐시된 형상과는 TShape 가 다르다
        const ShapeMap empty_map;
        const ShapeMap& current = steps.empty() || !steps.back().maps.count(type) ? empty_map : steps.back().maps.at(type);
        const int index = p["index"].get<int>();
        if (index < 1 || index > current.Extent())
          throw Error("out_of_range", "없는 엔티티 번호입니다", {{"param", "index"}, {"count", current.Extent()}});
        const TopoDS_Shape target = current(index);
        Json present = Json::array();
        Id created_by = 0;
        for (const Step& st : steps) {
          auto it = st.maps.find(type);
          if (it != st.maps.end() && contains(it->second, target)) {
            if (!created_by) created_by = st.feature;
            present.push_back(st.feature);
          }
        }
        return Json{{"type", type}, {"index", index}, {"created_by", created_by}, {"present_in", present}};
      }
      Json out = Json::array();
      for (const Step& st : steps) {
        Json row{{"feature", st.feature}, {"name", st.name}, {"state", st.state}};
        for (const auto& [name, kind] : kinds) {
          auto cur = st.maps.find(name);
          const ShapeMap empty;
          const ShapeMap& now = cur == st.maps.end() ? empty : cur->second;
          const ShapeMap& before = prev.count(name) ? prev[name] : empty;
          int created = 0, removed = 0;
          for (int i = 1; i <= now.Extent(); ++i)
            if (!contains(before, now(i))) ++created;
          for (int i = 1; i <= before.Extent(); ++i)
            if (!contains(now, before(i))) ++removed;
          row[name + "s"] = Json{{"count", now.Extent()}, {"created", created}, {"removed", removed}};
        }
        prev = st.maps;
        out.push_back(std::move(row));
      }
      return Json{{"id", part.id}, {"features", out}};
    };
    app.register_command(std::move(c));
  }
  // ------------------------------------------------------------ 스케치 명령(GEO-33·37·38). 구속(GEO-34~36)은 해석기 결정(D8) 뒤에
  {
    auto sketch_of = [](App& a, const Json& p) -> Object {
      const Object& o = a.model().get(p["id"].get<Id>());
      if (o.kind != "sketch") throw Error("wrong_kind", "스케치가 아닙니다", {{"object", o.id}, {"expected", "sketch"}});
      return o;
    };
    auto next_id = [](const Object& sk) {
      int n = 0;
      for (const Json& e : sk.props.value("entities", Json::array())) n = std::max(n, e.value("id", 0));
      return n + 1;
    };
    auto add_entity = [sketch_of, next_id](App& a, const Json& p, const std::string& kind, const std::vector<const char*>& keys) {
      Object sk = sketch_of(a, p);
      Json e{{"kind", kind}, {"id", next_id(sk)}};
      for (const char* k : keys) {
        if (!has(p, k)) throw Error("missing_param", std::string("필수 매개변수가 없습니다: ") + k, {{"param", k}});
        e[k] = p[k];
      }
      if (has(p, "angle")) e["angle"] = p["angle"];
      if (p.value("reference", false)) e["reference"] = true;
      sketch_edges(sketch_frame(a, sk), e);  // 만들어지는지 지금 확인한다
      Json ents = sk.props.value("entities", Json::array());
      ents.push_back(e);
      sk.props["entities"] = ents;
      a.model().replace(sk);
      return Json{{"id", sk.id}, {"entity", e["id"]}, {"count", ents.size()}};
    };
    auto uv = [](const char* name, const char* desc) { return F(name, "number_list", desc).call_req().ex({0.0, 0.0}); };
    struct Def {
      const char* name;
      const char* desc;
      std::vector<const char*> keys;
      Fields params;
    };
    const std::vector<Def> defs = {
        {"line", "선분", {"start", "end"}, {uv("start", "시작점 [u, v]"), uv("end", "끝점 [u, v]")}},
        {"circle", "원", {"center", "radius"}, {uv("center", "중심 [u, v]"), F("radius", "number", "반지름").call_req().gt(0).ex(5.0)}},
        {"arc", "원호(세 점)", {"start", "middle", "end"}, {uv("start", "시작점 [u, v]"), uv("middle", "호 위의 점 [u, v]"), uv("end", "끝점 [u, v]")}},
        {"rectangle", "사각형", {"corner", "size"}, {uv("corner", "한 꼭짓점 [u, v]"), F("size", "number_list", "[너비, 높이]").call_req().ex({10.0, 5.0})}},
        {"ellipse", "타원", {"center", "radii"}, {uv("center", "중심 [u, v]"), F("radii", "number_list", "[큰 반지름, 작은 반지름]").call_req().ex({5.0, 3.0}), F("angle", "number", "큰 축의 각도(도)").ex(0.0)}},
        {"spline", "스플라인", {"points"}, {F("points", "table", "지나는 점 [[u, v], …]").columns(2).call_req().ex(Json::array({Json::array({0.0, 0.0}), Json::array({5.0, 3.0}), Json::array({10.0, 0.0})}))}},
        {"point", "점", {"position"}, {uv("position", "[u, v]")}},
    };
    for (const Def& d : defs) {
      CommandSpec c = base(std::string("sketch.add_") + d.name, 'C', "sketch", std::string("스케치에 ") + d.desc + "을(를) 더한다(좌표는 스케치 평면의 u·v). 요소 번호를 돌려준다", "GEO-33");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch")};
      for (const F& f : d.params) c.params.push_back(f);
      c.params.push_back(F("reference", "bool", "참조 요소로 둔다(단면에 쓰지 않음)"));
      const std::string kind = d.name;
      const std::vector<const char*> keys = d.keys;
      c.fn = [add_entity, kind, keys](App& a, const Json& p) { return add_entity(a, p, kind, keys); };
      app.register_command(std::move(c));
    }
    // ---- 구속(GEO-34~36): 자체 해석기(sketch_solver.cpp). 구속을 더하거나 치수를 바꾸면 바로 풀어 요소 좌표를 고친다
    auto solve_and_store = [sketch_of](App& a, Object& sk, bool identify_conflicts) {
      Json ents = sk.props.value("entities", Json::array());
      const Json cons = sk.props.value("constraints", Json::array());
      const SketchSolveResult r = solve_sketch(ents, cons, identify_conflicts);
      if (r.converged) sk.props["entities"] = ents;
      a.model().replace(sk);
      Json out{{"status", r.status}, {"converged", r.converged}, {"residual", r.residual}, {"variables", r.variables}, {"equations", r.equations},
               {"rank", r.rank}, {"dof", r.dof}, {"redundant", r.redundant}, {"conflicts", r.conflicts}, {"iterations", r.iterations}};
      return out;
    };
    auto next_constraint_id = [](const Object& sk) {
      int n = 0;
      for (const Json& c : sk.props.value("constraints", Json::array())) n = std::max(n, c.value("id", 0));
      return n + 1;
    };
    auto add_constraint = [sketch_of, solve_and_store, next_constraint_id](App& a, const Json& p, bool dimension) {
      Object sk = sketch_of(a, p);
      const std::string kind = p["kind"].get<std::string>();
      static const std::set<std::string> geometric = {"coincident", "horizontal", "vertical", "parallel", "perpendicular", "tangent", "equal", "concentric", "fixed", "symmetric", "point_on_line", "point_on_circle"};
      static const std::set<std::string> dims = {"length", "distance", "horizontal_distance", "vertical_distance", "angle", "radius", "diameter"};
      if (dimension ? !dims.count(kind) : !geometric.count(kind))
        throw Error("invalid_choice", std::string(dimension ? "치수" : "기하 구속") + " 종류가 아닙니다: " + kind, {{"param", "kind"}});
      if (dimension && !has(p, "value")) throw Error("missing_param", "치수에는 value 가 필요합니다", {{"param", "value"}});
      Json c{{"id", next_constraint_id(sk)}, {"kind", kind}};
      for (const char* k : {"entities", "points", "value", "at", "internal"})
        if (has(p, k)) c[k] = p[k];
      if (kind == "fixed" && !has(p, "at")) {  // 지금 자리에 고정
        if (!has(p, "points") || p["points"].empty()) throw Error("missing_param", "fixed 구속에는 점이 필요합니다", {{"param", "points"}});
        const auto at = sketch_point_position(sk.props.value("entities", Json::array()), p["points"][0]);
        c["at"] = Json::array({at[0], at[1]});
      }
      Json cons = sk.props.value("constraints", Json::array());
      cons.push_back(c);
      sk.props["constraints"] = cons;
      // 구속 정의가 틀리면(없는 요소·점) 여기서 Error → 명령 전체가 되돌려진다
      Json out = solve_and_store(a, sk, true);
      out["id"] = sk.id, out["constraint"] = c["id"], out["count"] = cons.size();
      return out;
    };
    {
      CommandSpec c = base("sketch.add_constraint", 'C', "sketch",
                           "기하 구속을 더하고 스케치를 푼다: coincident(점 2)·horizontal/vertical(선분 1 또는 점 2)·parallel/perpendicular(선분 2)·tangent(선분-원/원호, 원-원)·"
                           "equal(선분 2 길이 또는 원 2 반지름)·concentric(원/원호 2)·fixed(점 1, at)·symmetric(점 2, 축 선분)·point_on_line(점, 선분)·point_on_circle(점, 원). "
                           "점은 {entity, point: start|end|center|middle|position}. 풀리지 않으면 상태 conflict 와 충돌 구속 번호를 돌려준다(요소는 그대로)",
                           "GEO-34");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"),
                  F("kind", "string", "구속 종류").call_req().one_of({"coincident", "horizontal", "vertical", "parallel", "perpendicular", "tangent", "equal", "concentric", "fixed", "symmetric", "point_on_line", "point_on_circle"}).ex("horizontal"),
                  F("entities", "integer_list", "대상 요소 번호").ge(1).ex(Json::array({1})),
                  F("points", "object_list", "대상 점 [{entity, point: start|end|center|middle|position}, …](점·원·사각형 요소는 point 생략 가능)")
                      .of({F("entity", "integer", "요소 번호").call_req().ge(1).ex(1), F("point", "string", "어느 점").ex("end")}),
                  F("at", "number_list", "fixed: 고정할 좌표 [u, v]").ex({0.0, 0.0}), F("internal", "bool", "tangent(원-원): 안쪽 접촉")};
      c.fn = [add_constraint](App& a, const Json& p) { return add_constraint(a, p, false); };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.add_dimension", 'C', "sketch",
                           "치수 구속을 더하고 스케치를 푼다: length(선분)·distance(점 2 또는 점-선분)·horizontal_distance/vertical_distance(점 2)·angle(선분 2, 도)·radius/diameter(원·원호)",
                           "GEO-35");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"),
                  F("kind", "string", "치수 종류").call_req().one_of({"length", "distance", "horizontal_distance", "vertical_distance", "angle", "radius", "diameter"}).ex("length"),
                  F("value", "number", "치수 값").call_req().ex(50.0),
                  F("entities", "integer_list", "대상 요소 번호").ge(1).ex(Json::array({1})),
                  F("points", "object_list", "대상 점 [{entity, point: start|end|center|middle|position}, …]")
                      .of({F("entity", "integer", "요소 번호").call_req().ge(1).ex(1), F("point", "string", "어느 점").ex("end")})};
      c.fn = [add_constraint](App& a, const Json& p) { return add_constraint(a, p, true); };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.set_dimension", 'C', "sketch", "치수 구속의 값을 바꾸고 스케치를 다시 푼다", "GEO-35");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("constraint", "integer", "구속 번호").call_req().ge(1).ex(1),
                  F("value", "number", "새 치수 값").call_req().ex(60.0)};
      c.fn = [sketch_of, solve_and_store](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        Json cons = sk.props.value("constraints", Json::array());
        bool found = false;
        for (Json& c : cons)
          if (c.value("id", 0) == p["constraint"].get<int>()) {
            if (!c.contains("value")) throw Error("invalid_param", "치수 구속이 아닙니다", {{"param", "constraint"}});
            c["value"] = p["value"], found = true;
          }
        if (!found) throw Error("not_found", "없는 구속입니다", {{"param", "constraint"}});
        sk.props["constraints"] = cons;
        Json out = solve_and_store(a, sk, true);
        out["id"] = sk.id, out["constraint"] = p["constraint"];
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.remove_constraint", 'C', "sketch", "구속을 지운다(구속 번호 목록). 남은 구속으로 다시 푼다", "GEO-34, GEO-35");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("constraints", "integer_list", "지울 구속 번호").call_req().ge(1).ex(Json::array({1}))};
      c.fn = [sketch_of, solve_and_store](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        std::set<int> gone;
        for (const Json& i : p["constraints"]) gone.insert(i.get<int>());
        Json kept = Json::array();
        std::size_t removed = 0;
        for (const Json& c : sk.props.value("constraints", Json::array())) {
          if (gone.count(c.value("id", 0))) ++removed;
          else kept.push_back(c);
        }
        if (!removed) throw Error("not_found", "지울 구속이 없습니다", {{"param", "constraints"}});
        sk.props["constraints"] = kept;
        Json out = solve_and_store(a, sk, true);
        out["id"] = sk.id, out["removed"] = removed, out["count"] = kept.size();
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.move_point", 'C', "sketch", "요소의 점을 옮기고(드래그) 구속을 다시 푼다. 구속이 허락하는 만큼만 움직인다", "GEO-33, GEO-34");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("entity", "integer", "요소 번호").call_req().ge(1).ex(1),
                  F("point", "string", "어느 점").call_req().one_of({"start", "end", "center", "middle", "position", "corner"}).ex("end"),
                  F("to", "number_list", "새 좌표 [u, v]").call_req().ex({10.0, 5.0})};
      c.fn = [sketch_of, solve_and_store](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        Json ents = sk.props.value("entities", Json::array());
        bool found = false;
        for (Json& e : ents)
          if (e.value("id", 0) == p["entity"].get<int>()) {
            const std::string key = p["point"].get<std::string>();
            if (!e.contains(key)) throw Error("invalid_param", "요소에 그 점이 없습니다: " + key, {{"param", "point"}});
            if (e.value("reference", false)) throw Error("invalid_state", "참조 요소는 옮길 수 없습니다", {{"param", "entity"}});
            e[key] = p["to"], found = true;
          }
        if (!found) throw Error("not_found", "없는 요소입니다", {{"param", "entity"}});
        sk.props["entities"] = ents;
        Json out = solve_and_store(a, sk, false);
        if (!out["converged"].get<bool>()) throw Error("conflict", "그 자리로는 구속을 만족시킬 수 없습니다", {{"param", "to"}, {"status", out}});
        out["id"] = sk.id;
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.solve", 'C', "sketch", "구속으로 스케치를 (다시) 푼다. 풀리면 요소 좌표를 고친다", "GEO-34, GEO-35, GEO-36");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch")};
      c.fn = [sketch_of, solve_and_store](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        Json out = solve_and_store(a, sk, true);
        out["id"] = sk.id;
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.solve_status", 'Q', "sketch",
                           "구속 상태를 조회한다: status = no_constraints | unconstrained(자유도 남음, dof) | fully_constrained | over_constrained(독립이 아닌 구속 redundant) | conflict(풀리지 않음, 충돌 구속 conflicts). "
                           "변수 수·식 수·계수(rank)도 준다. 모델은 바꾸지 않는다",
                           "GEO-36");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch")};
      c.fn = [sketch_of](App& a, const Json& p) {
        const Object sk = sketch_of(a, p);
        Json ents = sk.props.value("entities", Json::array());
        const SketchSolveResult r = solve_sketch(ents, sk.props.value("constraints", Json::array()), true);
        return Json{{"id", sk.id}, {"status", r.status}, {"converged", r.converged}, {"residual", r.residual}, {"variables", r.variables}, {"equations", r.equations},
                    {"rank", r.rank}, {"dof", r.dof}, {"redundant", r.redundant}, {"conflicts", r.conflicts}, {"constraints", sk.props.value("constraints", Json::array()).size()}};
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.remove", 'C', "sketch", "스케치 요소를 지운다(요소 번호). 그 요소를 쓰는 구속도 함께 지운다", "GEO-33");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("entities", "integer_list", "지울 요소 번호").call_req().ge(1).ex({1})};
      c.fn = [sketch_of](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        std::set<int> gone;
        for (const Json& i : p["entities"]) gone.insert(i.get<int>());
        Json kept = Json::array();
        std::size_t removed = 0;
        for (const Json& e : sk.props.value("entities", Json::array())) {
          if (gone.count(e.value("id", 0))) ++removed;
          else kept.push_back(e);
        }
        if (!removed) throw Error("not_found", "지울 요소가 없습니다", {{"param", "entities"}});
        sk.props["entities"] = kept;
        Json cons = Json::array();
        std::size_t dropped = 0;
        for (const Json& c : sk.props.value("constraints", Json::array())) {
          bool uses = false;
          for (const Json& e : c.value("entities", Json::array())) if (gone.count(e.get<int>())) uses = true;
          for (const Json& pt : c.value("points", Json::array()))
            if (gone.count(pt.is_object() ? pt.value("entity", 0) : pt.is_array() ? pt[0].get<int>() : pt.get<int>())) uses = true;
          if (uses) ++dropped;
          else cons.push_back(c);
        }
        if (sk.props.contains("constraints")) sk.props["constraints"] = cons;
        a.model().replace(sk);
        return Json{{"id", sk.id}, {"removed", removed}, {"count", kept.size()}, {"constraints_removed", dropped}};
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.edit", 'C', "sketch",
                           "스케치 요소를 편집한다: trim(선분의 구간 자르기: entity·start·end 0~1), extend(선분 늘리기: entity·length·at), fillet(두 선분 사이 둥글리기: entities 2개·radius — 두 선분을 줄이고 호를 더함), "
                           "mirror(요소들을 축 선분으로 대칭 복사: entities·axis(선분 요소 번호)), offset(원·선분을 거리만큼: entities·distance)",
                           "GEO-33");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("operation", "string", "편집 종류").call_req().one_of({"trim", "extend", "fillet", "mirror", "offset"}).ex("trim"),
                  F("entity", "integer", "대상 요소 번호").ge(1), F("entities", "integer_list", "대상 요소 번호들").ge(1),
                  F("start", "number", "trim: 남길 구간 시작(0~1)").ge(0).le(1), F("end", "number", "trim: 남길 구간 끝(0~1)").ge(0).le(1),
                  F("length", "number", "extend: 늘릴 길이").gt(0), F("at", "string", "extend: 늘릴 쪽").one_of({"start", "end", "both"}),
                  F("radius", "number", "fillet: 반지름").gt(0), F("axis", "integer", "mirror: 대칭축이 되는 선분 요소 번호").ge(1),
                  F("distance", "number", "offset: 거리(원은 반지름에 더함, 선분은 왼쪽 법선 쪽)")};
      c.fn = [sketch_of, next_id](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        check_value(a.commands().at("sketch.edit").params[1], p["operation"], nullptr);
        const std::string op = p["operation"].get<std::string>();
        Json ents = sk.props.value("entities", Json::array());
        auto find = [&](int id) -> Json& {
          for (Json& e : ents)
            if (e.value("id", 0) == id) return e;
          throw Error("not_found", "없는 스케치 요소입니다: " + std::to_string(id), {{"param", "entity"}});
        };
        auto lerp = [](const Json& a_, const Json& b_, double t) {
          return Json::array({a_[0].get<double>() + t * (b_[0].get<double>() - a_[0].get<double>()), a_[1].get<double>() + t * (b_[1].get<double>() - a_[1].get<double>())});
        };
        Json made = Json::array();
        if (op == "trim" || op == "extend") {
          if (!has(p, "entity")) throw Error("missing_param", "entity 가 필요합니다", {{"param", "entity"}});
          Json& e = find(p["entity"].get<int>());
          if (e["kind"] != "line") throw Error("invalid_param", "trim·extend 는 선분에만 됩니다", {{"param", "entity"}});
          const Json s0 = e["start"], e0 = e["end"];
          if (op == "trim") {
            const double t0 = p.value("start", 0.0), t1 = p.value("end", 1.0);
            if (t0 >= t1) throw Error("out_of_range", "start 는 end 보다 작아야 합니다", {{"param", "end"}});
            e["start"] = lerp(s0, e0, t0), e["end"] = lerp(s0, e0, t1);
          } else {
            const double len = p.value("length", 0.0);
            if (len <= 0) throw Error("missing_param", "length 가 필요합니다", {{"param", "length"}});
            const double L = std::hypot(e0[0].get<double>() - s0[0].get<double>(), e0[1].get<double>() - s0[1].get<double>());
            if (L <= 0) throw Error("invalid_geometry", "길이가 0 인 선분입니다", {{"param", "entity"}});
            const std::string at = p.value("at", std::string("end"));
            if (at != "start") e["end"] = lerp(s0, e0, 1.0 + len / L);
            if (at != "end") e["start"] = lerp(s0, e0, -len / L);
          }
          made.push_back(e["id"]);
        } else if (op == "fillet") {
          const Json ids = p.value("entities", Json::array());
          if (ids.size() != 2 || !has(p, "radius")) throw Error("missing_param", "fillet 에는 선분 둘(entities)과 radius 가 필요합니다", {{"param", "entities"}});
          Json& l1 = find(ids[0].get<int>());
          Json& l2 = find(ids[1].get<int>());
          if (l1["kind"] != "line" || l2["kind"] != "line") throw Error("invalid_param", "fillet 은 선분 둘 사이에만 됩니다", {{"param", "entities"}});
          // 공통 꼭짓점(가장 가까운 끝점 쌍)을 찾아 두 선분을 r/tan(θ/2) 만큼 줄이고 접점 사이에 호를 둔다
          const double r = p["radius"].get<double>();
          double best = 1e300;
          const char* k1 = "start";
          const char* k2 = "start";
          for (const char* a_ : {"start", "end"})
            for (const char* b_ : {"start", "end"}) {
              const double d = std::hypot(l1[a_][0].get<double>() - l2[b_][0].get<double>(), l1[a_][1].get<double>() - l2[b_][1].get<double>());
              if (d < best) best = d, k1 = a_, k2 = b_;
            }
          if (best > 1e-9) throw Error("invalid_geometry", "두 선분이 끝점에서 만나지 않습니다", {{"param", "entities"}});
          const Json corner = l1[k1];
          const char* o1 = std::string(k1) == "start" ? "end" : "start";
          const char* o2 = std::string(k2) == "start" ? "end" : "start";
          const double ax = l1[o1][0].get<double>() - corner[0].get<double>(), ay = l1[o1][1].get<double>() - corner[1].get<double>();
          const double bx = l2[o2][0].get<double>() - corner[0].get<double>(), by = l2[o2][1].get<double>() - corner[1].get<double>();
          const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
          const double cosang = (ax * bx + ay * by) / (la * lb);
          const double theta = std::acos(std::clamp(cosang, -1.0, 1.0));
          if (theta < 1e-6 || theta > kPi - 1e-6) throw Error("invalid_geometry", "두 선분이 나란해 둥글릴 수 없습니다", {{"param", "entities"}});
          const double cut = r / std::tan(0.5 * theta);
          if (cut >= la || cut >= lb) throw Error("out_of_range", "반지름이 너무 큽니다", {{"param", "radius"}});
          const Json t1 = Json::array({corner[0].get<double>() + ax / la * cut, corner[1].get<double>() + ay / la * cut});
          const Json t2 = Json::array({corner[0].get<double>() + bx / lb * cut, corner[1].get<double>() + by / lb * cut});
          // 호의 가운데 점: 이등분선 위, 꼭짓점에서 r/sin(θ/2) − r 만큼
          const double mx = ax / la + bx / lb, my = ay / la + by / lb, ml = std::hypot(mx, my);
          const double dist = r / std::sin(0.5 * theta) - r;
          const Json mid = Json::array({corner[0].get<double>() + mx / ml * dist, corner[1].get<double>() + my / ml * dist});
          l1[k1] = t1, l2[k2] = t2;
          Json arc{{"kind", "arc"}, {"id", next_id(sk)}, {"start", t1}, {"middle", mid}, {"end", t2}};
          Object probe = sk;
          probe.props["entities"] = ents;
          sketch_edges(sketch_frame(a, probe), arc);
          ents.push_back(arc);
          made.push_back(arc["id"]);
        } else if (op == "mirror") {
          if (!has(p, "axis")) throw Error("missing_param", "axis(선분 요소 번호)가 필요합니다", {{"param", "axis"}});
          const Json axis = find(p["axis"].get<int>());
          if (axis["kind"] != "line") throw Error("invalid_param", "대칭축은 선분이어야 합니다", {{"param", "axis"}});
          const double px = axis["start"][0].get<double>(), py = axis["start"][1].get<double>();
          double dx = axis["end"][0].get<double>() - px, dy = axis["end"][1].get<double>() - py;
          const double dl = std::hypot(dx, dy);
          if (dl <= 0) throw Error("invalid_geometry", "대칭축의 길이가 0 입니다", {{"param", "axis"}});
          dx /= dl, dy /= dl;
          auto mir = [&](const Json& q) {
            const double vx = q[0].get<double>() - px, vy = q[1].get<double>() - py, t = vx * dx + vy * dy;
            return Json::array({px + 2 * t * dx - vx, py + 2 * t * dy - vy});
          };
          std::vector<Json> copies;
          for (const Json& i : p.value("entities", Json::array())) {
            Json e = find(i.get<int>());
            for (const char* key : {"start", "end", "middle", "center", "position"})
              if (has(e, key)) e[key] = mir(e[key]);
            if (has(e, "points")) {
              Json pts = Json::array();
              for (const Json& q : e["points"]) pts.push_back(mir(q));
              e["points"] = pts;
            }
            if (e["kind"] == "rectangle") {  // 사각형은 꼭짓점 넷을 선분 넷으로
              const double u0 = e["corner"][0].get<double>(), v0 = e["corner"][1].get<double>(), w = e["size"][0].get<double>(), h = e["size"][1].get<double>();
              const Json c[4] = {Json::array({u0, v0}), Json::array({u0 + w, v0}), Json::array({u0 + w, v0 + h}), Json::array({u0, v0 + h})};
              for (int k = 0; k < 4; ++k) copies.push_back(Json{{"kind", "line"}, {"start", mir(c[k])}, {"end", mir(c[(k + 1) % 4])}});
              continue;
            }
            if (e["kind"] == "ellipse") e["angle"] = std::atan2(dy, dx) * 360.0 / kPi - e.value("angle", 0.0);  // 축에 대칭한 각
            copies.push_back(e);
          }
          if (copies.empty()) throw Error("missing_param", "entities 가 필요합니다", {{"param", "entities"}});
          for (Json& e : copies) {
            e["id"] = next_id(sk);
            ents.push_back(e);
            sk.props["entities"] = ents;
            made.push_back(e["id"]);
          }
        } else {  // offset
          if (!has(p, "distance")) throw Error("missing_param", "distance 가 필요합니다", {{"param", "distance"}});
          const double d = p["distance"].get<double>();
          std::vector<Json> copies;
          for (const Json& i : p.value("entities", Json::array())) {
            Json e = find(i.get<int>());
            if (e["kind"] == "circle") {
              if (e["radius"].get<double>() + d <= 0) throw Error("out_of_range", "오프셋한 반지름이 0 이하입니다", {{"param", "distance"}});
              e["radius"] = e["radius"].get<double>() + d;
            } else if (e["kind"] == "line") {
              const double dx = e["end"][0].get<double>() - e["start"][0].get<double>(), dy = e["end"][1].get<double>() - e["start"][1].get<double>();
              const double L = std::hypot(dx, dy);
              if (L <= 0) throw Error("invalid_geometry", "길이가 0 인 선분입니다", {{"param", "entities"}});
              const double nx = -dy / L * d, ny = dx / L * d;
              e["start"] = Json::array({e["start"][0].get<double>() + nx, e["start"][1].get<double>() + ny});
              e["end"] = Json::array({e["end"][0].get<double>() + nx, e["end"][1].get<double>() + ny});
            } else {
              throw Error("invalid_param", "offset 은 원·선분에만 됩니다", {{"param", "entities"}});
            }
            copies.push_back(e);
          }
          if (copies.empty()) throw Error("missing_param", "entities 가 필요합니다", {{"param", "entities"}});
          for (Json& e : copies) {
            e["id"] = next_id(sk);
            ents.push_back(e);
            sk.props["entities"] = ents;
            made.push_back(e["id"]);
          }
        }
        sk.props["entities"] = ents;
        a.model().replace(sk);
        return Json{{"id", sk.id}, {"operation", op}, {"entities", made}, {"count", ents.size()}};
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.project", 'C', "sketch", "파트 형상의 모서리·꼭짓점을 스케치 평면에 투영해 참조 요소(선분·원·꺾은선 스플라인·점)로 더한다. 평면과 나란한 직선·원은 그대로, 그 밖의 곡선은 점을 찍어 스플라인으로", "GEO-37");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("part", "ref", "투영할 파트").call_req().ref("part"),
                  F("edges", "integer_list", "모서리 번호(없으면 전부)").ge(1), F("vertices", "integer_list", "꼭짓점 번호").ge(1)};
      c.fn = [sketch_of, next_id](App& a, const Json& p) {
        Object sk = sketch_of(a, p);
        const SketchFrame f = sketch_frame(a, sk);
        const PartShape& ps = solid_shape(a, a.model().get(p["part"].get<Id>()));
        auto to_uv = [&](const gp_Pnt& q) {
          const gp_Vec d(f.origin, q);
          return Json::array({d.Dot(gp_Vec(f.u)), d.Dot(gp_Vec(f.v))});
        };
        Json ents = sk.props.value("entities", Json::array());
        Json made = Json::array();
        auto push = [&](Json e) {
          e["id"] = next_id(sk), e["reference"] = true;
          ents.push_back(e);
          sk.props["entities"] = ents;
          made.push_back(e["id"]);
        };
        std::vector<int> edge_ids;
        if (has(p, "edges")) for (const Json& i : p["edges"]) edge_ids.push_back(i.get<int>());
        else for (int i = 1; i <= ps.edges.Extent(); ++i) edge_ids.push_back(i);
        for (int i : edge_ids) {
          const TopoDS_Edge e = TopoDS::Edge(entity_of(ps.edges, i, "edge"));
          if (BRep_Tool::Degenerated(e)) continue;
          BRepAdaptor_Curve crv(e);
          if (crv.GetType() == GeomAbs_Line) {
            push(Json{{"kind", "line"}, {"start", to_uv(crv.Value(crv.FirstParameter()))}, {"end", to_uv(crv.Value(crv.LastParameter()))}});
          } else if (crv.GetType() == GeomAbs_Circle && std::fabs(crv.Circle().Axis().Direction().Dot(f.normal)) > 1.0 - 1e-9 &&
                     std::fabs(crv.LastParameter() - crv.FirstParameter() - 2.0 * kPi) < 1e-9) {
            push(Json{{"kind", "circle"}, {"center", to_uv(crv.Circle().Location())}, {"radius", crv.Circle().Radius()}});
          } else {
            Json pts = Json::array();
            const int n = 16;
            for (int k = 0; k <= n; ++k) pts.push_back(to_uv(crv.Value(crv.FirstParameter() + (crv.LastParameter() - crv.FirstParameter()) * k / n)));
            push(Json{{"kind", "spline"}, {"points", pts}});
          }
        }
        for (const Json& i : p.value("vertices", Json::array()))
          push(Json{{"kind", "point"}, {"position", to_uv(BRep_Tool::Pnt(TopoDS::Vertex(entity_of(ps.vertices, i.get<int>(), "vertex"))))}});
        if (made.empty()) throw Error("invalid_state", "투영할 모서리·꼭짓점이 없습니다", {{"param", "part"}});
        a.model().replace(sk);
        return Json{{"id", sk.id}, {"entities", made}, {"count", ents.size()}};
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.profiles", 'Q', "sketch", "스케치의 닫힌 영역(단면으로 쓸 수 있는 면) 목록을 조회한다: 번호(피처의 profile)·넓이·무게중심(u, v)·둘레 모서리 수", "GEO-38");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch")};
      c.fn = [sketch_of](App& a, const Json& p) {
        const Object sk = sketch_of(a, p);
        const SketchFrame f = sketch_frame(a, sk);
        Json out = Json::array();
        int index = 0;
        for (const TopoDS_Face& face : sketch_profiles(a, sk)) {
          GProp_GProps g;
          BRepGProp::SurfaceProperties(face, g);
          const gp_Vec d(f.origin, g.CentreOfMass());
          int edges = 0;
          for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) ++edges;
          out.push_back(Json{{"profile", ++index}, {"area", g.Mass()}, {"center", Json::array({d.Dot(gp_Vec(f.u)), d.Dot(gp_Vec(f.v))})}, {"edges", edges}});
        }
        return Json{{"id", sk.id}, {"count", out.size()}, {"profiles", out}};
      };
      app.register_command(std::move(c));
    }
    // ------------------------------------------------------------ 그리기 보조(마우스 스케치 모드용. 렌더러가 아니라 명령 계층에서 계산한다)
    auto frame_json = [](const SketchFrame& f) {
      auto v3 = [](const gp_Pnt& q) { return Json::array({q.X(), q.Y(), q.Z()}); };
      auto d3 = [](const gp_Dir& d) { return Json::array({d.X(), d.Y(), d.Z()}); };
      return Json{{"origin", v3(f.origin)}, {"u", d3(f.u)}, {"v", d3(f.v)}, {"normal", d3(f.normal)}};
    };
    auto uv_of = [](const SketchFrame& f, const gp_Pnt& q) {
      const gp_Vec d(f.origin, q);
      return Json::array({d.Dot(gp_Vec(f.u)), d.Dot(gp_Vec(f.v))});
    };
    {
      CommandSpec c = base("sketch.frame", 'Q', "sketch", "스케치 평면의 3D 틀(원점, u·v 축, 법선)을 조회한다. (u, v) 를 주면 그 점의 xyz 도, point 를 주면 평면에 수직 투영한 (u, v) 도 돌려준다",
                           "GEO-32, RND-38");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("uv", "number_list", "평면 좌표 [u, v] → xyz").ex({10.0, 5.0}),
                  F("point", "vector3", "공간의 점 → 평면에 투영한 [u, v]")};
      c.fn = [sketch_of, frame_json, uv_of](App& a, const Json& p) {
        const Object sk = sketch_of(a, p);
        const SketchFrame f = sketch_frame(a, sk);
        Json out = frame_json(f);
        out["id"] = sk.id;
        if (has(p, "uv")) {
          if (p["uv"].size() != 2) throw Error("invalid_param", "uv 는 [u, v] 여야 합니다", {{"param", "uv"}});
          const gp_Pnt q = sketch_pt(f, p["uv"]);
          out["point"] = Json::array({q.X(), q.Y(), q.Z()});
        }
        if (has(p, "point")) {
          const gp_Pnt q(p["point"][0].get<double>(), p["point"][1].get<double>(), p["point"][2].get<double>());
          out["uv"] = uv_of(f, q);
          out["distance"] = gp_Vec(f.origin, q).Dot(gp_Vec(f.normal));  // 평면에서 떨어진 거리(부호 = 법선 쪽)
        }
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      // 카메라 규약은 렌더러(view.cpp look_at/projection)와 같다: 오른쪽 = unit(f × up), 위 = 오른쪽 × f, y 픽셀은 아래로, 원근은 세로 시야각(도),
      // 직교는 height 가 화면에 담기는 세로 길이. 픽셀 중심(x + 0.5, y + 0.5)을 쓴다.
      CommandSpec c = base("sketch.point_from_screen", 'Q', "sketch",
                           "화면 픽셀(x, y)에서 카메라 광선을 쏘아 스케치 평면과 만나는 점의 (u, v)·xyz 를 구한다(마우스로 그리기). 카메라는 view.camera_get 의 것(렌더러가 없으면 camera 매개변수)",
                           "GEO-32, RND-38");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("x", "number", "가로 위치(픽셀, 왼쪽이 0)").call_req().ex(400.0),
                  F("y", "number", "세로 위치(픽셀, 위가 0)").call_req().ex(300.0), F("width", "integer", "화면 너비(픽셀)").call_req().ge(1).ex(800),
                  F("height", "integer", "화면 높이(픽셀)").call_req().ge(1).ex(600),
                  F("camera", "object", "카메라 {eye, target, up, projection: orthographic|perspective, fov(도), height}. 없으면 view.camera_get")};
      c.fn = [sketch_of, uv_of](App& a, const Json& p) {
        const Object sk = sketch_of(a, p);
        const SketchFrame f = sketch_frame(a, sk);
        Json cam;
        if (has(p, "camera")) cam = p["camera"];
        else if (a.commands().count("view.camera_get")) cam = a.commands().at("view.camera_get").fn(a, Json::object());
        else throw Error("not_available", "카메라가 없습니다(렌더러 없음): camera 매개변수를 주십시오", {{"param", "camera"}});
        for (const char* key : {"eye", "target", "up"})
          if (!has(cam, key) || !cam[key].is_array() || cam[key].size() != 3) throw Error("invalid_param", std::string("camera.") + key + " 가 없습니다", {{"param", "camera"}});
        auto v = [&](const char* key) { return gp_Vec(cam[key][0].get<double>(), cam[key][1].get<double>(), cam[key][2].get<double>()); };
        const gp_Vec eye = v("eye"), target = v("target"), up = v("up");
        gp_Vec fwd = target - eye;
        if (fwd.Magnitude() < 1e-12) throw Error("invalid_param", "camera 의 eye 와 target 이 같습니다", {{"param", "camera"}});
        fwd.Normalize();
        gp_Vec right = fwd.Crossed(up);
        if (right.Magnitude() < 1e-12) right = fwd.Crossed(std::fabs(fwd.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(0, 1, 0));
        right.Normalize();
        const gp_Vec upv = right.Crossed(fwd);
        const double w = p["width"].get<double>(), h = p["height"].get<double>();
        const double x = p["x"].get<double>(), y = p["y"].get<double>();
        const double ndc_x = 2.0 * (x + 0.5) / w - 1.0, ndc_y = 1.0 - 2.0 * (y + 0.5) / h, aspect = w / h;
        gp_Vec origin = eye, dir = fwd;
        if (cam.value("projection", std::string("perspective")) == "orthographic") {
          const double hh = 0.5 * cam.value("height", 100.0), hw = hh * aspect;
          origin = eye + right * (ndc_x * hw) + upv * (ndc_y * hh);
        } else {
          const double t = std::tan(0.5 * cam.value("fov", 45.0) * kPi / 180.0);
          dir = fwd + right * (ndc_x * t * aspect) + upv * (ndc_y * t);
          dir.Normalize();
        }
        const gp_Vec n(f.normal);
        const double denom = dir.Dot(n);
        Json out{{"id", sk.id}, {"ray", Json{{"origin", {origin.X(), origin.Y(), origin.Z()}}, {"direction", {dir.X(), dir.Y(), dir.Z()}}}}};
        if (std::fabs(denom) < 1e-9) {  // 광선이 평면과 나란하다(평면을 옆에서 보는 중)
          out["hit"] = false;
          return out;
        }
        const double t = (gp_Vec(f.origin.XYZ()) - origin).Dot(n) / denom;
        const gp_Vec q = origin + dir * t;
        const gp_Pnt qp(q.X(), q.Y(), q.Z());
        out["hit"] = true, out["behind"] = t < 0;  // 카메라 뒤쪽 교점(직교 투영에서는 뜻이 없다)
        out["point"] = Json::array({q.X(), q.Y(), q.Z()});
        out["uv"] = uv_of(f, qp);
        out["grazing"] = std::fabs(denom) < 0.17;  // 10° 안쪽으로 비스듬히 보면 작은 마우스 움직임이 큰 좌표 변화가 된다
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.snap", 'Q', "sketch",
                           "평면 좌표 (u, v) 를 가까운 특징점에 맞춘다: 요소의 끝점·중심·중점·꼭짓점(endpoint·center·midpoint), 기준점과의 수평·수직 정렬(horizontal·vertical), 격자(grid). "
                           "허용 거리 안의 가장 가까운 것을 고른다(특징점 > 정렬 > 격자). 없으면 그대로",
                           "GEO-32, GEO-34, RND-38");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("uv", "number_list", "맞출 점 [u, v]").call_req().ex({10.2, 4.9}),
                  F("tolerance", "number", "허용 거리(평면 단위)").call_req().gt(0).ex(0.5),
                  F("from", "number_list", "기준점 [u, v](선을 긋는 중이면 시작점): 이 점과 수평·수직으로 맞춘다").ex({0.0, 0.0}),
                  F("grid", "number", "격자 간격(0 또는 없으면 격자 스냅 없음)").ge(0).ex(1.0),
                  F("exclude", "integer", "제외할 요소 번호(그리는 중인 요소)").ge(1)};
      c.fn = [sketch_of](App& a, const Json& p) {
        const Object sk = sketch_of(a, p);
        if (p["uv"].size() != 2) throw Error("invalid_param", "uv 는 [u, v] 여야 합니다", {{"param", "uv"}});
        const double u = p["uv"][0].get<double>(), v = p["uv"][1].get<double>(), tol = p["tolerance"].get<double>();
        const int exclude = p.value("exclude", 0);
        struct Cand {
          double u, v, d;
          std::string kind;
          int entity;
          std::string point;
        };
        std::vector<Cand> cands;
        auto consider = [&](double cu, double cv, const char* kind, int entity, const char* point) {
          const double d = std::hypot(cu - u, cv - v);
          if (d <= tol) cands.push_back({cu, cv, d, kind, entity, point});
        };
        auto at = [](const Json& q) { return std::pair<double, double>{q[0].get<double>(), q[1].get<double>()}; };
        for (const Json& e : sk.props.value("entities", Json::array())) {
          const int id = e.value("id", 0);
          if (id == exclude) continue;
          const std::string kind = e.value("kind", std::string());
          if (kind == "line") {
            auto [u0, v0] = at(e["start"]);
            auto [u1, v1] = at(e["end"]);
            consider(u0, v0, "endpoint", id, "start"), consider(u1, v1, "endpoint", id, "end");
            consider(0.5 * (u0 + u1), 0.5 * (v0 + v1), "midpoint", id, "middle");
          } else if (kind == "arc") {
            auto [u0, v0] = at(e["start"]);
            auto [u1, v1] = at(e["end"]);
            auto [um, vm] = at(e["middle"]);
            consider(u0, v0, "endpoint", id, "start"), consider(u1, v1, "endpoint", id, "end"), consider(um, vm, "midpoint", id, "middle");
            // 호의 중심(세 점의 외심)
            const double bx = u1 - u0, by = v1 - v0, cx = um - u0, cy = vm - v0;
            const double d = 2.0 * (bx * cy - by * cx);
            if (std::fabs(d) > 1e-12) {
              const double b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
              consider(u0 + (cy * b2 - by * c2) / d, v0 + (bx * c2 - cx * b2) / d, "center", id, "center");
            }
          } else if (kind == "circle" || kind == "ellipse") {
            auto [uc, vc] = at(e["center"]);
            consider(uc, vc, "center", id, "center");
            if (kind == "circle") {
              const double r = e["radius"].get<double>();
              for (const auto& [du, dv] : std::vector<std::pair<double, double>>{{r, 0}, {-r, 0}, {0, r}, {0, -r}}) consider(uc + du, vc + dv, "quadrant", id, "quadrant");
            }
          } else if (kind == "rectangle") {
            auto [u0, v0] = at(e["corner"]);
            auto [w, h] = at(e["size"]);
            const double cs[4][2] = {{u0, v0}, {u0 + w, v0}, {u0 + w, v0 + h}, {u0, v0 + h}};
            for (int k = 0; k < 4; ++k) consider(cs[k][0], cs[k][1], "endpoint", id, "corner");
            consider(u0 + 0.5 * w, v0 + 0.5 * h, "center", id, "center");
          } else if (kind == "spline") {
            for (const Json& q : e.value("points", Json::array())) consider(q[0].get<double>(), q[1].get<double>(), "endpoint", id, "point");
          } else if (kind == "point") {
            auto [u0, v0] = at(e["position"]);
            consider(u0, v0, "endpoint", id, "position");
          }
        }
        consider(0.0, 0.0, "origin", 0, "origin");
        Json out{{"id", sk.id}, {"input", {u, v}}};
        if (!cands.empty()) {
          const Cand& best = *std::min_element(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
          out["uv"] = {best.u, best.v}, out["kind"] = best.kind, out["distance"] = best.d;
          if (best.entity) out["entity"] = best.entity, out["point"] = best.point;
          return out;
        }
        if (has(p, "from") && p["from"].size() == 2) {  // 기준점과 수평·수직 정렬
          const double fu = p["from"][0].get<double>(), fv = p["from"][1].get<double>();
          const double dh = std::fabs(v - fv), dv = std::fabs(u - fu);
          if (dh <= tol || dv <= tol) {
            const bool horizontal = dh <= dv;
            out["uv"] = horizontal ? Json::array({u, fv}) : Json::array({fu, v});
            out["kind"] = horizontal ? "horizontal" : "vertical", out["distance"] = horizontal ? dh : dv;
            return out;
          }
        }
        const double grid = p.value("grid", 0.0);
        if (grid > 0) {
          const double gu = std::round(u / grid) * grid, gv = std::round(v / grid) * grid;
          const double d = std::hypot(gu - u, gv - v);
          if (d <= tol) {
            out["uv"] = {gu, gv}, out["kind"] = "grid", out["distance"] = d;
            return out;
          }
        }
        out["uv"] = {u, v}, out["kind"] = "none", out["distance"] = 0.0;
        return out;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("sketch.tessellate", 'Q', "sketch",
                           "스케치 요소들을 화면에 그릴 3D 꺾은선으로 편다(요소마다 점 목록. 원·호·타원·스플라인은 각도 허용치로 분할). 평면 격자 범위와 틀도 함께 — 렌더러(RND-38)·REST 클라이언트가 그리는 데 쓴다",
                           "GEO-32, RND-38");
      c.params = {F("id", "ref", "스케치").call_req().ref("sketch"), F("deflection", "number", "곡선 분할 허용 거리(기본: 범위의 1/500)").gt(0),
                  F("angle", "number", "곡선 분할 허용 각(도, 기본 5)").gt(0).le(90)};
      c.fn = [sketch_of, frame_json](App& a, const Json& p) {
        const Object sk = sketch_of(a, p);
        const SketchFrame f = sketch_frame(a, sk);
        Json ents = Json::array();
        double umin = 0, umax = 0, vmin = 0, vmax = 0;
        bool any = false;
        auto grow = [&](const gp_Pnt& q) {
          const gp_Vec d(f.origin, q);
          const double u = d.Dot(gp_Vec(f.u)), v = d.Dot(gp_Vec(f.v));
          if (!any) umin = umax = u, vmin = vmax = v, any = true;
          umin = std::min(umin, u), umax = std::max(umax, u), vmin = std::min(vmin, v), vmax = std::max(vmax, v);
        };
        // 범위를 먼저 재서 분할 허용 거리의 기본값을 정한다
        std::vector<std::pair<Json, std::vector<TopoDS_Shape>>> made;
        for (const Json& e : sk.props.value("entities", Json::array())) {
          std::vector<TopoDS_Shape> shapes = sketch_edges(f, e);
          for (const TopoDS_Shape& sh : shapes)
            for (TopExp_Explorer ex(sh, TopAbs_VERTEX); ex.More(); ex.Next()) grow(BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())));
          made.push_back({e, std::move(shapes)});
        }
        const double extent = any ? std::max({umax - umin, vmax - vmin, 1e-6}) : 1.0;
        const double deflection = p.value("deflection", extent / 500.0), angle = p.value("angle", 5.0) * kPi / 180.0;
        for (auto& [e, shapes] : made) {
          Json lines = Json::array();
          for (const TopoDS_Shape& sh : shapes) {
            Json pts = Json::array();
            if (sh.ShapeType() == TopAbs_VERTEX) {
              const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(sh));
              pts.push_back(Json::array({q.X(), q.Y(), q.Z()}));
            } else {
              BRepAdaptor_Curve curve(TopoDS::Edge(sh));
              GCPnts_TangentialDeflection td(curve, angle, deflection);
              for (int i = 1; i <= td.NbPoints(); ++i) {
                const gp_Pnt q = td.Value(i);
                pts.push_back(Json::array({q.X(), q.Y(), q.Z()}));
              }
            }
            lines.push_back(pts);
          }
          ents.push_back(Json{{"id", e.value("id", 0)}, {"kind", e.value("kind", std::string())}, {"reference", e.value("reference", false)}, {"polylines", lines}});
        }
        Json out{{"id", sk.id}, {"frame", frame_json(f)}, {"entities", ents}};
        // 격자 범위: 요소 범위를 10% 넓힌 사각형(요소가 없으면 원점 둘레 ±100)
        const double pad = any ? 0.1 * extent : 0.0;
        out["bounds"] = any ? Json{{"u", {umin - pad, umax + pad}}, {"v", {vmin - pad, vmax + pad}}} : Json{{"u", {-100.0, 100.0}}, {"v", {-100.0, 100.0}}};
        // 치수 표식 위치: 치수 구속마다 대상의 가운데 점(화면에 값을 적을 자리)
        Json dims = Json::array();
        for (const Json& cst : sk.props.value("constraints", Json::array())) {
          if (!has(cst, "value")) continue;
          Json ref = Json::array();
          for (const Json& pt : cst.value("points", Json::array())) {
            try {
              const auto pos = sketch_point_position(sk.props["entities"], pt);
              ref.push_back(Json::array({pos[0], pos[1]}));
            } catch (const Error&) {
            }
          }
          dims.push_back(Json{{"id", cst.value("id", 0)}, {"kind", cst.value("kind", std::string())}, {"value", cst["value"]}, {"points", ref}});
        }
        out["dimensions"] = dims;
        return out;
      };
      app.register_command(std::move(c));
    }
  }
  {
    CommandSpec c = base("feature.regenerate", 'J', "part", "이력을 처음부터 다시 계산한다", "GEO-40");
    c.params = {F("id", "ref", "파트").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Id part = part_of(a, p).id;
      store(a).parts.erase(part);
      const PartShape& ps = shape_of(a, part);
      return Json{{"ok", ps.ok}, {"features", ps.status}};
    };
    app.register_command(std::move(c));
  }
}

#else  // OpenCASCADE 없이 빌드

bool geometry_available() { return false; }
std::string geometry_digest(App&, Id) { return ""; }
Json geometry_counts(App&, Id) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
std::string geometry_entity_name(App&, Id, const std::string&, int) { return std::string(); }
int geometry_entity_index(App&, Id, const std::string&, const std::string&) { return 0; }
std::vector<double> geometry_vertices(App&, Id) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
std::vector<int> geometry_nearest_edges(App&, Id, const std::vector<double>&) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
BlockTopology geometry_block_topology(App&, Id, int) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
std::vector<double> geometry_edge_points(App&, Id, int, const std::array<double, 3>&, int) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
void geometry_project_to_face(App&, Id, int, std::vector<double>&) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
void geometry_project_to_edge(App&, Id, int, std::vector<double>&) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }
std::vector<double> geometry_edge_points_at(App&, Id, int, const std::array<double, 3>&, const std::vector<double>&) { throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다"); }

Tessellation geometry_tessellation(App&, Id, double, double) {
  throw Error("not_available", "이 빌드에는 형상 커널(OpenCASCADE)이 없습니다");
}

void register_geometry_commands(App&) {}

#endif

}  // namespace ofep
