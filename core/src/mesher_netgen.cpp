// Netgen(LGPL) 연결: OpenCASCADE 형상을 삼각형·사면체로 메싱한다. Netgen 을 쓰는 유일한 파일이다.
#include <atomic>
#include <mutex>

#include "ofep/error.hpp"
#include "ofep/mesher.hpp"

#ifdef OFEP_WITH_NETGEN
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#ifndef OCCGEOMETRY
#define OCCGEOMETRY  // Netgen 의 OpenCASCADE 연결부 헤더는 이 정의가 있어야 열린다
#endif
#include <meshing.hpp>
#include <occgeom.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <BRepBuilderAPI_Copy.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>

#include "geometry_occt.hpp"

namespace ofep {

bool mesher_available() { return true; }
std::string mesher_name() { return "netgen"; }

namespace {
// 번호표(BuildFMap)는 3차원 규칙으로 만들고 차원만 따로 정한다(SetDimension 은 번호표를 다시 만든다).
struct Geometry : netgen::OCCGeometry {
  explicit Geometry(const TopoDS_Shape& s) : netgen::OCCGeometry(s, 3, false) {}
  void set_dimension(int d) { dimension = d; }
};
// Netgen 은 진행률·중단 플래그를 전역(ngcore::multithread)에 두므로 메싱은 한 번에 하나만 돈다.
std::mutex& mesher_mutex() {
  static std::mutex m;
  return m;
}
std::atomic<bool> g_running{false};
}  // namespace

MesherGeometry prepare_mesher_geometry(App& app, Id part) {
  // 핸들을 복사해 둔다: 파트가 나중에 바뀌거나 지워져도 이 형상은 그대로 남는다.
  return {std::make_shared<const TopoDS_Shape>(geometry_shape(app, part))};
}

MesherProgress mesher_progress() {
  MesherProgress p;
  p.running = g_running.load();
  if (!p.running) return p;
  p.percent = ngcore::multithread.percent;
  if (const char* t = ngcore::multithread.task) p.task = t;
  return p;
}

void mesher_cancel() {
  if (g_running.load()) ngcore::multithread.terminate = 1;
}

MesherOutput run_mesher(App& app, Id part, const MesherRequest& rq) { return run_mesher(prepare_mesher_geometry(app, part), rq); }

MesherOutput run_mesher(const MesherGeometry& geometry, const MesherRequest& rq) {
  if (!geometry.shape) throw Error("invalid_state", "메싱할 형상이 없습니다");
  const TopoDS_Shape& shape = *static_cast<const TopoDS_Shape*>(geometry.shape.get());
  std::unique_lock<std::mutex> lock(mesher_mutex(), std::try_to_lock);
  if (!lock.owns_lock()) throw Error("busy", "다른 메싱이 돌고 있습니다");
  netgen::printmessage_importance = 0;  // 진행 메시지를 표준 출력에 쓰지 않는다
  ngcore::multithread.terminate = 0;
  ngcore::multithread.percent = 0;
  ngcore::multithread.task = "";
  g_running = true;
  struct Done {
    ~Done() { g_running = false, ngcore::multithread.terminate = 0; }
  } done;
  MesherOutput out;
  try {
    // 형상을 복사해 넘긴다(메셔가 형상에 삼각화 등을 붙여도 우리 형상은 그대로다).
    // Netgen 의 copy=true 는 STEP 파일을 거쳐 복사하므로 솔리드 사이의 공유 면이 끊긴다(절점이 안 맞는 메시가 된다) → 직접 복사한다.
    BRepBuilderAPI_Copy copier(shape);
    const TopoDS_Shape copy = copier.Shape();
    // 차원은 번호표를 만든 뒤에 정한다: 2 로 만들면 Netgen 이 닫힌 껍질의 모서리에 안팎 면을 하나씩만 붙여
    // 첫 면의 경계 절점이 떨어진다(솔버로 확인: 자유 모서리가 생긴다). Netgen 의 STEP 경유 복사도 같은 순서였다.
    auto geom = std::make_shared<Geometry>(copy);
    geom->set_dimension(rq.dimension);
    netgen::MeshingParameters mp;
    mp.maxh = rq.max_size;
    mp.minh = rq.min_size;
    mp.grading = rq.grading;
    mp.perfstepsend = rq.dimension == 3 ? netgen::MESHCONST_OPTVOLUME : netgen::MESHCONST_OPTSURFACE;
    if (rq.curvature_safety > 0) mp.curvaturesafety = rq.curvature_safety;
    // 엔티티별 크기: 우리 번호(TopExp::MapShapes 순서)의 원본 엔티티 → 복사본 엔티티 → Netgen 의 그 엔티티. Netgen 자체 번호 순서와 다를 수 있어 번호를 쓰지 않는다
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces), TopExp::MapShapes(shape, TopAbs_EDGE, edges), TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
    auto bad = [](const char* what, int i) { return Error("not_found", std::string("없는 엔티티입니다: ") + what + " " + std::to_string(i), {{"param", what}}); };
    for (const auto& [face, h] : rq.face_sizes) {
      if (face < 1 || face > faces.Extent()) throw bad("face", face);
      geom->GetFace(copier.ModifiedShape(faces(face))).properties.maxh = h;
    }
    for (const auto& [edge, h] : rq.edge_sizes) {
      if (edge < 1 || edge > edges.Extent()) throw bad("edge", edge);
      geom->GetEdge(copier.ModifiedShape(edges(edge))).properties.maxh = h;
    }
    for (const auto& [vertex, h] : rq.vertex_sizes) {
      if (vertex < 1 || vertex > vertices.Extent()) throw bad("vertex", vertex);
      geom->GetVertex(copier.ModifiedShape(vertices(vertex))).properties.maxh = h;
    }
    auto mesh = std::make_shared<netgen::Mesh>();
    mesh->SetGeometry(geom);
    const int rc = geom->GenerateMesh(mesh, mp);
    if (ngcore::multithread.terminate) throw Error("cancelled", "메싱을 멈췄습니다");
    if (rc != 0) throw Error("mesh_failed", "메셔가 메시를 만들지 못했습니다");
    if (rq.order == 2) geom->GetRefinement().MakeSecondOrder(*mesh);

    // 절점: 메셔의 번호 → 0 부터 센 위치
    netgen::Array<std::int64_t, netgen::PointIndex> at(mesh->GetNP());
    std::int64_t n = 0;
    for (netgen::PointIndex pi : mesh->Points().Range()) {
      const netgen::MeshPoint& p = (*mesh)[pi];
      out.xyz.insert(out.xyz.end(), {p(0), p(1), p(2)});
      at[pi] = n++;
    }
    for (netgen::ElementIndex ei : mesh->VolumeElements().Range()) {
      const auto el = (*mesh)[ei];
      if (el.IsDeleted()) continue;
      out.volume.nodes_per = el.GetNP();
      for (netgen::PointIndex pi : el.PNums()) out.volume.conn.push_back(at[pi]);
      out.volume.tag.push_back(el.GetIndex().Nr1());
    }
    for (netgen::SurfaceElementIndex si : mesh->SurfaceElements().Range()) {
      const auto el = (*mesh)[si];
      if (el.IsDeleted()) continue;
      out.surface.nodes_per = el.GetNP();
      for (netgen::PointIndex pi : el.PNums()) out.surface.conn.push_back(at[pi]);
      out.surface.tag.push_back(mesh->GetFaceDescriptor(el).SurfNr());
    }
    for (netgen::SegmentIndex si : mesh->LineSegments().Range()) {
      const netgen::Segment& seg = (*mesh)[si];
      const bool mid = rq.order == 2 && seg[2].IsValid();
      out.edge.nodes_per = mid ? 3 : 2;
      out.edge.conn.push_back(at[seg[0]]);
      out.edge.conn.push_back(at[seg[1]]);
      if (mid) out.edge.conn.push_back(at[seg[2]]);
      out.edge.tag.push_back(seg.GetIndex().Nr1());
    }
  } catch (const Error&) {
    throw;
  } catch (const ngcore::Exception& e) {
    if (ngcore::multithread.terminate) throw Error("cancelled", "메싱을 멈췄습니다");
    throw Error("mesh_failed", std::string("메셔 오류: ") + e.What());
  } catch (const Standard_Failure& e) {
    throw Error("mesh_failed", std::string("메셔(형상 커널) 오류: ") + e.what());
  } catch (const std::exception& e) {
    throw Error("mesh_failed", std::string("메셔 오류: ") + e.what());
  }
  if (out.xyz.empty() || (rq.dimension == 3 ? out.volume.tag.empty() : out.surface.tag.empty()))
    throw Error("mesh_failed", "메셔가 요소를 만들지 못했습니다(형상이나 요소 크기를 확인)");
  return out;
}

}  // namespace ofep

#else  // Netgen 없이 빌드

namespace ofep {

bool mesher_available() { return false; }
std::string mesher_name() { return ""; }
MesherGeometry prepare_mesher_geometry(App&, Id) { throw Error("not_available", "이 빌드에는 자동 메셔(Netgen)가 없습니다"); }
MesherOutput run_mesher(const MesherGeometry&, const MesherRequest&) { throw Error("not_available", "이 빌드에는 자동 메셔(Netgen)가 없습니다"); }
MesherOutput run_mesher(App&, Id, const MesherRequest&) { throw Error("not_available", "이 빌드에는 자동 메셔(Netgen)가 없습니다"); }
MesherProgress mesher_progress() { return {}; }
void mesher_cancel() {}

}  // namespace ofep

#endif
