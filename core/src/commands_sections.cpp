// 표준 형강 목록(PRP-05, D15): KS·EN 의 H·ㄷ·ㄱ 형강, 각형·원형 강관을 내장 표로 두고 보 프로퍼티로 가져온다.
// 치수는 mm(mm-t-s). 가져올 때 모델 단위계로 환산한다. 단면 종류는 property.create_beam 의 section 과 같다(I·C·L·box·pipe).
// 출처: KS D 3503(H 형강 — JIS G 3192 와 같은 치수), KS D 3502(ㄱ·ㄷ 형강), KS D 3568(각형강관), KS D 3566(강관),
//      EN 10365(IPE·HEA·HEB·UPN), EN 10056-1(ㄱ 형강), EN 10219(CHS·SHS·RHS). 공칭 치수(h, b, tw, tf / a, b, t / D, t)만 수록하고
//      필렛 반지름은 쓰지 않는다(합성보 분해는 직사각형만 쓴다).
#include <algorithm>
#include <cmath>
#include <set>

#include "nasa95/app.hpp"
#include "nasa95/error.hpp"

namespace nasa95 {

namespace {

using F = FieldSpec;

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }

struct SectionRow {
  const char* standard;  // KS | EN
  const char* series;    // H, C(ㄷ), L(ㄱ), SHS/RHS(각형강관), CHS(강관), IPE, HEA, HEB, UPN
  const char* name;
  const char* section;   // I | C | L | box | pipe
  double d[4];           // I·C·L: h, b, tw, tf / box: a(1축), b(2축), t / pipe: D(바깥 지름), t
};

// clang-format off
const SectionRow kSections[] = {
  // --- KS D 3503 H 형강(H-h×b×tw×tf). 광폭(정사각 계열)
  {"KS","H","H-100x100x6x8","I",{100,100,6,8}}, {"KS","H","H-125x125x6.5x9","I",{125,125,6.5,9}}, {"KS","H","H-150x150x7x10","I",{150,150,7,10}},
  {"KS","H","H-175x175x7.5x11","I",{175,175,7.5,11}}, {"KS","H","H-200x200x8x12","I",{200,200,8,12}}, {"KS","H","H-250x250x9x14","I",{250,250,9,14}},
  {"KS","H","H-300x300x10x15","I",{300,300,10,15}}, {"KS","H","H-350x350x12x19","I",{350,350,12,19}}, {"KS","H","H-400x400x13x21","I",{400,400,13,21}},
  // 중폭
  {"KS","H","H-250x175x7x11","I",{250,175,7,11}}, {"KS","H","H-300x200x8x12","I",{300,200,8,12}}, {"KS","H","H-350x250x9x14","I",{350,250,9,14}},
  {"KS","H","H-400x300x10x16","I",{400,300,10,16}}, {"KS","H","H-450x300x11x18","I",{450,300,11,18}}, {"KS","H","H-500x300x11x18","I",{500,300,11,18}},
  {"KS","H","H-600x300x12x20","I",{600,300,12,20}},
  // 세폭
  {"KS","H","H-150x75x5x7","I",{150,75,5,7}}, {"KS","H","H-200x100x5.5x8","I",{200,100,5.5,8}}, {"KS","H","H-250x125x6x9","I",{250,125,6,9}},
  {"KS","H","H-300x150x6.5x9","I",{300,150,6.5,9}}, {"KS","H","H-350x175x7x11","I",{350,175,7,11}}, {"KS","H","H-400x200x8x13","I",{400,200,8,13}},
  {"KS","H","H-450x200x9x14","I",{450,200,9,14}}, {"KS","H","H-500x200x10x16","I",{500,200,10,16}}, {"KS","H","H-600x200x11x17","I",{600,200,11,17}},
  {"KS","H","H-700x300x13x24","I",{700,300,13,24}}, {"KS","H","H-800x300x14x26","I",{800,300,14,26}}, {"KS","H","H-900x300x16x28","I",{900,300,16,28}},
  // --- KS D 3502 ㄷ 형강(C-h×b×tw×tf)
  {"KS","C","C-75x40x5x7","C",{75,40,5,7}}, {"KS","C","C-100x50x5x7.5","C",{100,50,5,7.5}}, {"KS","C","C-125x65x6x8","C",{125,65,6,8}},
  {"KS","C","C-150x75x6.5x10","C",{150,75,6.5,10}}, {"KS","C","C-150x75x9x12.5","C",{150,75,9,12.5}}, {"KS","C","C-180x75x7x10.5","C",{180,75,7,10.5}},
  {"KS","C","C-200x80x7.5x11","C",{200,80,7.5,11}}, {"KS","C","C-200x90x8x13.5","C",{200,90,8,13.5}}, {"KS","C","C-250x90x9x13","C",{250,90,9,13}},
  {"KS","C","C-250x90x11x14.5","C",{250,90,11,14.5}}, {"KS","C","C-300x90x9x13","C",{300,90,9,13}}, {"KS","C","C-300x90x10x15.5","C",{300,90,10,15.5}},
  {"KS","C","C-300x90x12x16","C",{300,90,12,16}}, {"KS","C","C-380x100x10.5x16","C",{380,100,10.5,16}}, {"KS","C","C-380x100x13x16.5","C",{380,100,13,16.5}},
  // --- KS D 3502 등변 ㄱ 형강(L-a×a×t)
  {"KS","L","L-40x40x3","L",{40,40,3,3}}, {"KS","L","L-40x40x5","L",{40,40,5,5}}, {"KS","L","L-45x45x4","L",{45,45,4,4}}, {"KS","L","L-50x50x4","L",{50,50,4,4}},
  {"KS","L","L-50x50x6","L",{50,50,6,6}}, {"KS","L","L-60x60x5","L",{60,60,5,5}}, {"KS","L","L-65x65x6","L",{65,65,6,6}}, {"KS","L","L-65x65x8","L",{65,65,8,8}},
  {"KS","L","L-70x70x6","L",{70,70,6,6}}, {"KS","L","L-75x75x6","L",{75,75,6,6}}, {"KS","L","L-75x75x9","L",{75,75,9,9}}, {"KS","L","L-75x75x12","L",{75,75,12,12}},
  {"KS","L","L-80x80x6","L",{80,80,6,6}}, {"KS","L","L-90x90x6","L",{90,90,6,6}}, {"KS","L","L-90x90x7","L",{90,90,7,7}}, {"KS","L","L-90x90x10","L",{90,90,10,10}},
  {"KS","L","L-90x90x13","L",{90,90,13,13}}, {"KS","L","L-100x100x7","L",{100,100,7,7}}, {"KS","L","L-100x100x10","L",{100,100,10,10}}, {"KS","L","L-100x100x13","L",{100,100,13,13}},
  {"KS","L","L-120x120x8","L",{120,120,8,8}}, {"KS","L","L-130x130x9","L",{130,130,9,9}}, {"KS","L","L-130x130x12","L",{130,130,12,12}}, {"KS","L","L-130x130x15","L",{130,130,15,15}},
  {"KS","L","L-150x150x12","L",{150,150,12,12}}, {"KS","L","L-150x150x15","L",{150,150,15,15}}, {"KS","L","L-150x150x19","L",{150,150,19,19}},
  {"KS","L","L-175x175x12","L",{175,175,12,12}}, {"KS","L","L-175x175x15","L",{175,175,15,15}}, {"KS","L","L-200x200x15","L",{200,200,15,15}},
  {"KS","L","L-200x200x20","L",{200,200,20,20}}, {"KS","L","L-200x200x25","L",{200,200,25,25}}, {"KS","L","L-250x250x25","L",{250,250,25,25}}, {"KS","L","L-250x250x35","L",{250,250,35,35}},
  // --- KS D 3568 각형강관(□-a×b×t)
  {"KS","SHS","SHS-50x50x2.3","box",{50,50,2.3}}, {"KS","SHS","SHS-50x50x3.2","box",{50,50,3.2}}, {"KS","SHS","SHS-60x60x3.2","box",{60,60,3.2}},
  {"KS","SHS","SHS-75x75x3.2","box",{75,75,3.2}}, {"KS","SHS","SHS-75x75x4.5","box",{75,75,4.5}}, {"KS","SHS","SHS-100x100x3.2","box",{100,100,3.2}},
  {"KS","SHS","SHS-100x100x4.5","box",{100,100,4.5}}, {"KS","SHS","SHS-100x100x6","box",{100,100,6}}, {"KS","SHS","SHS-125x125x4.5","box",{125,125,4.5}},
  {"KS","SHS","SHS-125x125x6","box",{125,125,6}}, {"KS","SHS","SHS-150x150x4.5","box",{150,150,4.5}}, {"KS","SHS","SHS-150x150x6","box",{150,150,6}},
  {"KS","SHS","SHS-150x150x9","box",{150,150,9}}, {"KS","SHS","SHS-175x175x6","box",{175,175,6}}, {"KS","SHS","SHS-200x200x6","box",{200,200,6}},
  {"KS","SHS","SHS-200x200x9","box",{200,200,9}}, {"KS","SHS","SHS-200x200x12","box",{200,200,12}}, {"KS","SHS","SHS-250x250x9","box",{250,250,9}},
  {"KS","SHS","SHS-250x250x12","box",{250,250,12}}, {"KS","SHS","SHS-300x300x9","box",{300,300,9}}, {"KS","SHS","SHS-300x300x12","box",{300,300,12}},
  {"KS","SHS","SHS-350x350x12","box",{350,350,12}}, {"KS","SHS","SHS-400x400x12","box",{400,400,12}}, {"KS","SHS","SHS-400x400x16","box",{400,400,16}},
  {"KS","RHS","RHS-100x50x3.2","box",{100,50,3.2}}, {"KS","RHS","RHS-125x75x4.5","box",{125,75,4.5}}, {"KS","RHS","RHS-150x100x4.5","box",{150,100,4.5}},
  {"KS","RHS","RHS-150x100x6","box",{150,100,6}}, {"KS","RHS","RHS-200x100x6","box",{200,100,6}}, {"KS","RHS","RHS-250x150x6","box",{250,150,6}},
  {"KS","RHS","RHS-250x150x9","box",{250,150,9}}, {"KS","RHS","RHS-300x200x9","box",{300,200,9}}, {"KS","RHS","RHS-400x200x12","box",{400,200,12}},
  // --- KS D 3566 일반 구조용 탄소 강관(○-D×t)
  {"KS","CHS","CHS-48.6x3.2","pipe",{48.6,3.2}}, {"KS","CHS","CHS-60.5x3.2","pipe",{60.5,3.2}}, {"KS","CHS","CHS-76.3x3.2","pipe",{76.3,3.2}},
  {"KS","CHS","CHS-89.1x3.2","pipe",{89.1,3.2}}, {"KS","CHS","CHS-101.6x4.2","pipe",{101.6,4.2}}, {"KS","CHS","CHS-114.3x4.5","pipe",{114.3,4.5}},
  {"KS","CHS","CHS-139.8x4.5","pipe",{139.8,4.5}}, {"KS","CHS","CHS-165.2x5","pipe",{165.2,5}}, {"KS","CHS","CHS-190.7x5.3","pipe",{190.7,5.3}},
  {"KS","CHS","CHS-216.3x5.8","pipe",{216.3,5.8}}, {"KS","CHS","CHS-267.4x6","pipe",{267.4,6}}, {"KS","CHS","CHS-318.5x6.9","pipe",{318.5,6.9}},
  {"KS","CHS","CHS-355.6x7.9","pipe",{355.6,7.9}}, {"KS","CHS","CHS-406.4x9.5","pipe",{406.4,9.5}}, {"KS","CHS","CHS-457.2x9.5","pipe",{457.2,9.5}},
  {"KS","CHS","CHS-508x9.5","pipe",{508,9.5}}, {"KS","CHS","CHS-609.6x9.5","pipe",{609.6,9.5}},
  // --- EN 10365 IPE
  {"EN","IPE","IPE 80","I",{80,46,3.8,5.2}}, {"EN","IPE","IPE 100","I",{100,55,4.1,5.7}}, {"EN","IPE","IPE 120","I",{120,64,4.4,6.3}},
  {"EN","IPE","IPE 140","I",{140,73,4.7,6.9}}, {"EN","IPE","IPE 160","I",{160,82,5,7.4}}, {"EN","IPE","IPE 180","I",{180,91,5.3,8}},
  {"EN","IPE","IPE 200","I",{200,100,5.6,8.5}}, {"EN","IPE","IPE 220","I",{220,110,5.9,9.2}}, {"EN","IPE","IPE 240","I",{240,120,6.2,9.8}},
  {"EN","IPE","IPE 270","I",{270,135,6.6,10.2}}, {"EN","IPE","IPE 300","I",{300,150,7.1,10.7}}, {"EN","IPE","IPE 330","I",{330,160,7.5,11.5}},
  {"EN","IPE","IPE 360","I",{360,170,8,12.7}}, {"EN","IPE","IPE 400","I",{400,180,8.6,13.5}}, {"EN","IPE","IPE 450","I",{450,190,9.4,14.6}},
  {"EN","IPE","IPE 500","I",{500,200,10.2,16}}, {"EN","IPE","IPE 550","I",{550,210,11.1,17.2}}, {"EN","IPE","IPE 600","I",{600,220,12,19}},
  // --- EN 10365 HEA
  {"EN","HEA","HEA 100","I",{96,100,5,8}}, {"EN","HEA","HEA 120","I",{114,120,5,8}}, {"EN","HEA","HEA 140","I",{133,140,5.5,8.5}},
  {"EN","HEA","HEA 160","I",{152,160,6,9}}, {"EN","HEA","HEA 180","I",{171,180,6,9.5}}, {"EN","HEA","HEA 200","I",{190,200,6.5,10}},
  {"EN","HEA","HEA 220","I",{210,220,7,11}}, {"EN","HEA","HEA 240","I",{230,240,7.5,12}}, {"EN","HEA","HEA 260","I",{250,260,7.5,12.5}},
  {"EN","HEA","HEA 280","I",{270,280,8,13}}, {"EN","HEA","HEA 300","I",{290,300,8.5,14}}, {"EN","HEA","HEA 320","I",{310,300,9,15.5}},
  {"EN","HEA","HEA 340","I",{330,300,9.5,16.5}}, {"EN","HEA","HEA 360","I",{350,300,10,17.5}}, {"EN","HEA","HEA 400","I",{390,300,11,19}},
  {"EN","HEA","HEA 450","I",{440,300,11.5,21}}, {"EN","HEA","HEA 500","I",{490,300,12,23}}, {"EN","HEA","HEA 550","I",{540,300,12.5,24}},
  {"EN","HEA","HEA 600","I",{590,300,13,25}},
  // --- EN 10365 HEB
  {"EN","HEB","HEB 100","I",{100,100,6,10}}, {"EN","HEB","HEB 120","I",{120,120,6.5,11}}, {"EN","HEB","HEB 140","I",{140,140,7,12}},
  {"EN","HEB","HEB 160","I",{160,160,8,13}}, {"EN","HEB","HEB 180","I",{180,180,8.5,14}}, {"EN","HEB","HEB 200","I",{200,200,9,15}},
  {"EN","HEB","HEB 220","I",{220,220,9.5,16}}, {"EN","HEB","HEB 240","I",{240,240,10,17}}, {"EN","HEB","HEB 260","I",{260,260,10,17.5}},
  {"EN","HEB","HEB 280","I",{280,280,10.5,18}}, {"EN","HEB","HEB 300","I",{300,300,11,19}}, {"EN","HEB","HEB 320","I",{320,300,11.5,20.5}},
  {"EN","HEB","HEB 340","I",{340,300,12,21.5}}, {"EN","HEB","HEB 360","I",{360,300,12.5,22.5}}, {"EN","HEB","HEB 400","I",{400,300,13.5,24}},
  {"EN","HEB","HEB 450","I",{450,300,14,26}}, {"EN","HEB","HEB 500","I",{500,300,14.5,28}}, {"EN","HEB","HEB 550","I",{550,300,15,29}},
  {"EN","HEB","HEB 600","I",{600,300,15.5,30}},
  // --- EN 10365 UPN(ㄷ)
  {"EN","UPN","UPN 80","C",{80,45,6,8}}, {"EN","UPN","UPN 100","C",{100,50,6,8.5}}, {"EN","UPN","UPN 120","C",{120,55,7,9}},
  {"EN","UPN","UPN 140","C",{140,60,7,10}}, {"EN","UPN","UPN 160","C",{160,65,7.5,10.5}}, {"EN","UPN","UPN 180","C",{180,70,8,11}},
  {"EN","UPN","UPN 200","C",{200,75,8.5,11.5}}, {"EN","UPN","UPN 220","C",{220,80,9,12.5}}, {"EN","UPN","UPN 240","C",{240,85,9.5,13}},
  {"EN","UPN","UPN 260","C",{260,90,10,14}}, {"EN","UPN","UPN 280","C",{280,95,10,15}}, {"EN","UPN","UPN 300","C",{300,100,10,16}},
  {"EN","UPN","UPN 320","C",{320,100,14,17.5}}, {"EN","UPN","UPN 350","C",{350,100,14,16}}, {"EN","UPN","UPN 400","C",{400,110,14,18}},
  // --- EN 10056-1 등변 ㄱ 형강
  {"EN","L","L 40x40x4","L",{40,40,4,4}}, {"EN","L","L 50x50x5","L",{50,50,5,5}}, {"EN","L","L 60x60x6","L",{60,60,6,6}}, {"EN","L","L 70x70x7","L",{70,70,7,7}},
  {"EN","L","L 80x80x8","L",{80,80,8,8}}, {"EN","L","L 90x90x9","L",{90,90,9,9}}, {"EN","L","L 100x100x10","L",{100,100,10,10}}, {"EN","L","L 120x120x12","L",{120,120,12,12}},
  {"EN","L","L 150x150x15","L",{150,150,15,15}}, {"EN","L","L 200x200x20","L",{200,200,20,20}},
  // --- EN 10219 CHS·SHS·RHS(냉간 성형 중공 단면)
  {"EN","CHS","CHS 33.7x2.6","pipe",{33.7,2.6}}, {"EN","CHS","CHS 42.4x2.6","pipe",{42.4,2.6}}, {"EN","CHS","CHS 48.3x3.2","pipe",{48.3,3.2}},
  {"EN","CHS","CHS 60.3x3.2","pipe",{60.3,3.2}}, {"EN","CHS","CHS 76.1x3.2","pipe",{76.1,3.2}}, {"EN","CHS","CHS 88.9x3.6","pipe",{88.9,3.6}},
  {"EN","CHS","CHS 114.3x3.6","pipe",{114.3,3.6}}, {"EN","CHS","CHS 139.7x4","pipe",{139.7,4}}, {"EN","CHS","CHS 168.3x5","pipe",{168.3,5}},
  {"EN","CHS","CHS 219.1x6.3","pipe",{219.1,6.3}}, {"EN","CHS","CHS 273x6.3","pipe",{273,6.3}}, {"EN","CHS","CHS 323.9x8","pipe",{323.9,8}},
  {"EN","CHS","CHS 355.6x8","pipe",{355.6,8}}, {"EN","CHS","CHS 406.4x10","pipe",{406.4,10}}, {"EN","CHS","CHS 457x10","pipe",{457,10}}, {"EN","CHS","CHS 508x10","pipe",{508,10}},
  {"EN","SHS","SHS 40x40x3","box",{40,40,3}}, {"EN","SHS","SHS 50x50x3","box",{50,50,3}}, {"EN","SHS","SHS 60x60x3","box",{60,60,3}},
  {"EN","SHS","SHS 70x70x3.6","box",{70,70,3.6}}, {"EN","SHS","SHS 80x80x4","box",{80,80,4}}, {"EN","SHS","SHS 90x90x4","box",{90,90,4}},
  {"EN","SHS","SHS 100x100x5","box",{100,100,5}}, {"EN","SHS","SHS 120x120x5","box",{120,120,5}}, {"EN","SHS","SHS 140x140x5","box",{140,140,5}},
  {"EN","SHS","SHS 150x150x6","box",{150,150,6}}, {"EN","SHS","SHS 160x160x6","box",{160,160,6}}, {"EN","SHS","SHS 180x180x8","box",{180,180,8}},
  {"EN","SHS","SHS 200x200x8","box",{200,200,8}}, {"EN","SHS","SHS 250x250x10","box",{250,250,10}}, {"EN","SHS","SHS 300x300x10","box",{300,300,10}},
  {"EN","RHS","RHS 100x50x3","box",{100,50,3}}, {"EN","RHS","RHS 120x60x4","box",{120,60,4}}, {"EN","RHS","RHS 150x100x5","box",{150,100,5}},
  {"EN","RHS","RHS 200x100x6","box",{200,100,6}}, {"EN","RHS","RHS 250x150x8","box",{250,150,8}}, {"EN","RHS","RHS 300x200x8","box",{300,200,8}},
};
// clang-format on

// 수록 치수(mm) → property.create_beam 의 dimensions(모델 단위)
std::vector<double> dims_of(const SectionRow& r, double k) {
  const std::string s = r.section;
  if (s == "box") return {r.d[0] * k, r.d[1] * k, r.d[2] * k, r.d[2] * k, r.d[2] * k, r.d[2] * k};
  if (s == "pipe") return {r.d[0] / 2 * k, r.d[1] * k};  // 바깥 반지름, 두께
  return {r.d[0] * k, r.d[1] * k, r.d[2] * k, r.d[3] * k};
}

Json row_json(const SectionRow& r, double k) {
  Json j{{"name", r.name}, {"standard", r.standard}, {"series", r.series}, {"section", r.section}, {"dimensions", dims_of(r, k)}};
  const std::string s = r.section;
  if (s == "box") j["a"] = r.d[0] * k, j["b"] = r.d[1] * k, j["t"] = r.d[2] * k;
  else if (s == "pipe") j["D"] = r.d[0] * k, j["t"] = r.d[1] * k;
  else j["h"] = r.d[0] * k, j["b"] = r.d[1] * k, j["tw"] = r.d[2] * k, j["tf"] = r.d[3] * k;
  return j;
}

double length_factor(const App& a) {
  const Object* st = a.find_settings();
  const std::string model = st ? st->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
  return unit_factor("mm-t-s", model, "length");
}

}  // namespace

void register_section_commands(App& app) {
  {
    CommandSpec c;
    c.name = "property.section_library_list", c.kind = 'Q', c.target = "property", c.features = "PRP-05";
    c.desc = "내장 표준 형강 목록(KS D 3503/3502/3568/3566, EN 10365/10056-1/10219: H·IPE·HEA·HEB, ㄷ·UPN, ㄱ·L, 각형강관 SHS·RHS, 강관 CHS)을 조회한다. "
             "치수는 모델 단위계로 환산한 값(수록은 mm). standard·series 로 거른다";
    c.params = {F("standard", "string", "규격").one_of({"KS", "EN"}).ex("KS"),
                F("series", "string", "계열(H, C, L, SHS, RHS, CHS, IPE, HEA, HEB, UPN)").ex("H"),
                F("section", "string", "단면 종류").one_of({"I", "C", "L", "box", "pipe"}).ex("I")};
    c.fn = [](App& a, const Json& p) {
      const double k = length_factor(a);
      Json out = Json::array();
      for (const SectionRow& r : kSections) {
        if (has(p, "standard") && p["standard"].get<std::string>() != r.standard) continue;
        if (has(p, "series") && p["series"].get<std::string>() != r.series) continue;
        if (has(p, "section") && p["section"].get<std::string>() != r.section) continue;
        out.push_back(row_json(r, k));
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = "property.section_library_import", c.kind = 'C', c.undoable = true, c.target = "property", c.features = "PRP-05";
    c.desc = "표준 형강 목록의 단면으로 보 프로퍼티를 만든다(property.create_beam 과 같다. 치수는 모델 단위계로 환산). 이름을 주지 않으면 형강 이름(공백은 _)";
    c.params = {F("section_name", "string", "형강 이름(property.section_library_list 의 name)").call_req().ex("H-400x200x8x13"),
                F("name", "string", "프로퍼티 이름(없으면 형강 이름)"),
                F("material", "ref", "재료").ref("material"),
                F("direction", "vector3", "단면 1축 방향(형강은 웨브·높이 방향)").ex({0.0, 0.0, 1.0}),
                F("offset1", "number", "1축 오프셋"), F("offset2", "number", "2축 오프셋"),
                F("target", "any", "할당할 요소(property.create_beam 의 target)")};
    c.fn = [](App& a, const Json& p) {
      const std::string want = p["section_name"].get<std::string>();
      const SectionRow* row = nullptr;
      for (const SectionRow& r : kSections)
        if (want == r.name) row = &r;
      if (!row) throw Error("not_found", "표준 형강 목록에 없는 이름입니다: " + want, {{"param", "section_name"}});
      std::string default_name = row->name;  // 솔버 이름 규칙(공백 없음): "IPE 80" → "IPE_80"
      std::replace(default_name.begin(), default_name.end(), ' ', '_');
      Json q{{"section", row->section}, {"dimensions", dims_of(*row, length_factor(a))}, {"name", has(p, "name") ? p["name"] : Json(default_name)}};
      for (const char* key : {"material", "direction", "offset1", "offset2", "target"})
        if (has(p, key)) q[key] = p[key];
      Json r = a.invoke("property.create_beam", q);
      r["section_name"] = row->name, r["standard"] = row->standard;
      return r;
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
