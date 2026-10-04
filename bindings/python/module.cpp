// Python 모듈 `_ofep`: 코어의 명령 계층만 노출한다.
// 경계에서는 JSON 문자열을 주고받고, 대용량 수치 자료만 numpy 배열로 넘긴다(아키텍처 규칙 1).
#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "ofep/app.hpp"
#include "ofep/error.hpp"
#include "ofep/geometry.hpp"
#include "ofep/results.hpp"
#ifdef OFEP_WITH_RENDER
#include "ofep/render.hpp"
#endif

namespace py = pybind11;

namespace {
PyObject* g_core_error = nullptr;

// 코어의 오류를 CoreError(JSON 문자열)로 바꿔 던진다. Python 쪽 래퍼가 OfepError 로 푼다.
template <typename Fn>
auto guarded(Fn&& fn) -> decltype(fn()) {
  try {
    return fn();
  } catch (const ofep::Error& e) {
    PyErr_SetString(g_core_error, e.to_json().dump().c_str());
    throw py::error_already_set();
  } catch (const py::error_already_set&) {
    throw;
  } catch (const std::exception& e) {
    const ofep::Error wrapped("internal", e.what());
    PyErr_SetString(g_core_error, wrapped.to_json().dump().c_str());
    throw py::error_already_set();
  }
}

// 코어의 배열을 읽기 전용 numpy 배열로 복사해 준다.
// (코어의 배열은 뒤의 명령에서 다시 할당될 수 있어, 메모리를 그대로 가리키는 보기는 주지 않는다.)
template <typename T>
py::array_t<T> to_numpy(const std::vector<T>& v) {
  py::array_t<T> arr(static_cast<py::ssize_t>(v.size()));
  if (!v.empty()) std::memcpy(arr.mutable_data(), v.data(), v.size() * sizeof(T));
  arr.attr("flags").attr("writeable") = false;
  return arr;
}

ofep::Arrays to_arrays(const py::dict& d) {
  ofep::Arrays out;
  for (auto item : d) {
    const std::string key = py::cast<std::string>(item.first);
    const py::array arr = py::array::ensure(item.second);
    if (!arr) throw ofep::Error("invalid_param_type", "'" + key + "' 는 numpy 배열이어야 합니다", {{"param", key}});
    if (arr.ndim() == 2) out.cols[key] = static_cast<int>(arr.shape(1));
    const char kind = arr.dtype().kind();
    if (kind == 'f') {
      auto a = py::array_t<double, py::array::c_style | py::array::forcecast>::ensure(arr);
      out.doubles[key].assign(a.data(), a.data() + a.size());
    } else if (kind == 'i' || kind == 'u') {
      auto a = py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>::ensure(arr);
      out.ints[key].assign(a.data(), a.data() + a.size());
    } else {
      throw ofep::Error("invalid_param_type", "'" + key + "' 는 숫자 배열이어야 합니다", {{"param", key}});
    }
  }
  return out;
}
}  // namespace

PYBIND11_MODULE(_ofep, m) {
  m.doc() = "open-fep core";
  static py::exception<ofep::Error> core_error(m, "CoreError");
  g_core_error = core_error.ptr();

  py::class_<ofep::App>(m, "App")
      .def(py::init([] {
        auto app = std::make_unique<ofep::App>();
#ifdef OFEP_WITH_RENDER
        ofep::register_view_commands(*app);  // 렌더러는 코어 밖에 있으므로 여기서 뷰 명령을 붙인다
#endif
        return app;
      }))
      .def(
          "execute",
          [](ofep::App& a, const std::string& name, const std::string& params_json, const py::dict& arrays) {
            return guarded([&] {
              const ofep::Json params = params_json.empty() ? ofep::Json::object() : ofep::Json::parse(params_json);
              if (arrays.empty()) return a.execute(name, params).dump();
              const ofep::Arrays blobs = to_arrays(arrays);
              return a.execute(name, params, &blobs).dump();
            });
          },
          py::arg("name"), py::arg("params_json") = std::string(), py::arg("arrays") = py::dict())
      .def("subscribe",
           [](ofep::App& a, std::function<void(std::string)> fn) {
             return a.subscribe([fn = std::move(fn)](const ofep::Json& ev) { fn(ev.dump()); });
           })
      .def("unsubscribe", &ofep::App::unsubscribe)
      // Python 에서 명령을 등록한다(확장·스크립트 실행). fn 은 매개변수 JSON 문자열을 받아 결과 JSON 문자열을 돌려준다.
      .def(
          "register_command",
          [](ofep::App& a, const std::string& name, const std::string& kind, const std::string& desc,
             const std::string& params_json, const std::string& features, bool composite,
             std::function<std::string(std::string)> fn) {
            guarded([&] {
              ofep::CommandSpec c;
              c.name = name, c.desc = desc, c.features = features;
              c.kind = kind.empty() ? 'S' : kind[0];
              c.composite = composite, c.external = true;
              c.undoable = composite && c.kind == 'C';  // 묶음으로 실행되므로 안의 명령들이 Undo 한 단계가 된다
              for (const ofep::Json& p : ofep::Json::parse(params_json)) {
                ofep::FieldSpec f(p.at("name").get<std::string>(), p.at("type").get<std::string>(),
                                  p.value("desc", std::string()));
                f.must = p.value("must", false);
                if (p.contains("choices")) f.choices = p["choices"].get<std::vector<std::string>>();
                if (p.contains("example")) f.example = p["example"];
                c.params.push_back(std::move(f));
              }
              c.fn = [fn = std::move(fn)](ofep::App&, const ofep::Json& p) {
                const std::string out = fn(p.dump());
                return out.empty() ? ofep::Json::object() : ofep::Json::parse(out);
              };
              a.register_command(std::move(c));
            });
          },
          py::arg("name"), py::arg("kind"), py::arg("desc"), py::arg("params_json"), py::arg("features"),
          py::arg("composite"), py::arg("fn"))
      .def("unregister_command",
           [](ofep::App& a, const std::string& name) { guarded([&] { a.unregister_command(name); }); })
      // 메시 배열 조회(API-12). 노드·요소는 ID 오름차순이다.
      .def("mesh_node_ids", [](const ofep::App& a) { return to_numpy(a.mesh().node_ids()); })
      .def("mesh_node_coords",
           [](const ofep::App& a) {
             py::array_t<double> arr = to_numpy(a.mesh().node_xyz());
             arr.attr("flags").attr("writeable") = true;
             arr.resize({static_cast<py::ssize_t>(a.mesh().node_count()), static_cast<py::ssize_t>(3)});
             arr.attr("flags").attr("writeable") = false;
             return arr;
           })
      .def("mesh_element_ids", [](const ofep::App& a) { return to_numpy(a.mesh().element_ids()); })
      .def("mesh_element_nodes",
           [](const ofep::App& a) {
             // (요소별 절점 시작 위치, 절점 ID 를 이어 붙인 배열)
             const ofep::Mesh& mesh = a.mesh();
             std::vector<std::int64_t> offset, conn;
             offset.reserve(mesh.element_count() + 1);
             offset.push_back(0);
             for (std::size_t i = 0; i < mesh.element_count(); ++i) {
               const ofep::Id* n = mesh.nodes_at(i);
               for (std::size_t k = 0; k < mesh.node_count_at(i); ++k) conn.push_back(static_cast<std::int64_t>(n[k]));
               offset.push_back(static_cast<std::int64_t>(conn.size()));
             }
             return py::make_tuple(to_numpy(offset), to_numpy(conn));
           })
      .def("mesh_element_shapes",
           [](const ofep::App& a) {
             std::vector<std::string> names;
             for (std::size_t i = 0; i < a.mesh().element_count(); ++i)
               names.push_back(ofep::shape_info(a.mesh().shape_at(i)).name);
             return names;
           })
      // 결과 배열 조회(API-12, API-32): (절점 번호, (절점 수 × 성분 수) 값). component 를 주면 1차원(파생량 포함).
      .def(
          "result_values",
          [](ofep::App& a, std::uint64_t id, int frame, const std::string& field, const std::string& component) {
            return guarded([&]() -> py::tuple {
              ofep::ResultFile& file = ofep::results(a).get(id);
              const ofep::ResultField& f = file.field(file.frame(frame), field);
              std::vector<std::int64_t> ids(f.ids.begin(), f.ids.end());
              if (!component.empty()) {
                const std::size_t nc = f.components.size();
                std::vector<double> v;
                const auto it = std::find(f.components.begin(), f.components.end(), component);
                if (it != f.components.end()) {
                  const std::size_t c = static_cast<std::size_t>(it - f.components.begin());
                  v.resize(f.count);
                  for (std::size_t i = 0; i < f.count; ++i) v[i] = f.data[i * nc + c];
                } else {
                  v = ofep::derived_values(f, component);
                }
                return py::make_tuple(to_numpy(ids), to_numpy(v));
              }
              py::array_t<double> arr = to_numpy(f.data);
              arr.attr("flags").attr("writeable") = true;
              arr.resize({static_cast<py::ssize_t>(f.count), static_cast<py::ssize_t>(f.components.size())});
              arr.attr("flags").attr("writeable") = false;
              return py::make_tuple(to_numpy(ids), arr);
            });
          },
          py::arg("id"), py::arg("frame"), py::arg("field"), py::arg("component") = std::string())
      // 표시용 삼각화(GEO-06): 점·법선(N×3), 삼각형(M×3), 삼각형의 면 번호(M), 모서리 점(K×3), 모서리 시작 위치
      .def(
          "geometry_tessellation",
          [](ofep::App& a, std::uint64_t part, double deflection, double angle) {
            return guarded([&]() -> py::tuple {
              const ofep::Tessellation t = ofep::geometry_tessellation(a, part, deflection, angle);
              auto shaped = [](const auto& v, py::ssize_t cols) {
                auto arr = to_numpy(v);
                arr.attr("flags").attr("writeable") = true;
                arr.resize({static_cast<py::ssize_t>(v.size()) / cols, cols});
                arr.attr("flags").attr("writeable") = false;
                return arr;
              };
              return py::make_tuple(shaped(t.points, 3), shaped(t.normals, 3), shaped(t.triangles, 3), to_numpy(t.triangle_face),
                                    shaped(t.edge_points, 3), to_numpy(t.edge_offsets));
            });
          },
          py::arg("part"), py::arg("deflection") = 0.0, py::arg("angle") = 0.0)
#ifdef OFEP_WITH_RENDER
      // 현재 뷰를 그린 이미지(RND-45, RND-48): 색 (높이, 너비, 4) uint8 과 ID 버퍼 (높이, 너비) uint32
      .def(
          "view_render",
          [](ofep::App& a, int width, int height) {
            return guarded([&]() -> py::tuple {
              const ofep::RenderImage img = ofep::render_view(a, width, height);
              py::array_t<std::uint8_t> rgba = to_numpy(img.rgba);
              rgba.attr("flags").attr("writeable") = true;
              rgba.resize({static_cast<py::ssize_t>(img.height), static_cast<py::ssize_t>(img.width), static_cast<py::ssize_t>(4)});
              rgba.attr("flags").attr("writeable") = false;
              py::array_t<std::uint32_t> ids = to_numpy(img.ids);
              ids.attr("flags").attr("writeable") = true;
              ids.resize({static_cast<py::ssize_t>(img.height), static_cast<py::ssize_t>(img.width)});
              ids.attr("flags").attr("writeable") = false;
              return py::make_tuple(rgba, ids);
            });
          },
          py::arg("width") = 800, py::arg("height") = 600)
      // 창 연동(RND-02): 네이티브 창 핸들을 붙이고 프레임을 낸다. UI(PySide6)가 쓴다
      .def("view_attach_window", [](ofep::App& a, std::uintptr_t hwnd) { guarded([&] { ofep::view_attach_window(a, reinterpret_cast<void*>(hwnd)); }); })
      .def("view_detach_window", [](ofep::App& a) { guarded([&] { ofep::view_detach_window(a); }); })
      .def("view_present",
           [](ofep::App& a) {
             return guarded([&] {
               const auto size = ofep::view_present(a);
               return py::make_tuple(size[0], size[1]);
             });
           })
      .def("view_pick_window", [](ofep::App& a, int x, int y) { return guarded([&] { return ofep::view_pick_window(a, x, y).dump(); }); })
      .def("view_orbit", [](ofep::App& a, double dx, double dy) { return guarded([&] { return ofep::view_orbit(a, dx, dy).dump(); }); })
      .def("view_pan", [](ofep::App& a, double dx, double dy) { return guarded([&] { return ofep::view_pan(a, dx, dy).dump(); }); })
      .def("view_hover", [](ofep::App& a, int x, int y) { return guarded([&] { return ofep::view_hover(a, x, y).dump(); }); })
      .def("view_zoom", [](ofep::App& a, double factor, double x, double y) { return guarded([&] { return ofep::view_zoom(a, factor, x, y).dump(); }); })
#endif
      .def_property_readonly_static("version", [](py::object) { return std::string(ofep::App::version()); })
      .def_property_readonly_static("api_version", [](py::object) { return std::string(ofep::App::api_version()); });
}
