#pragma once
#include <cctype>
#include <map>
#include "nasa95/render.hpp"

namespace nasa95::render_detail {

inline const char* glyph(char c) {
  static const std::map<char, const char*> font = {
      {'0', "01110100011001110101110011000101110"}, {'1', "00100011000010000100001000010001110"}, {'2', "01110100010000100010001000100011111"},
      {'3', "11111000100010000010000011000101110"}, {'4', "00010001100101010010111110001000010"}, {'5', "11111100001111000001000011000101110"},
      {'6', "00110010001000011110100011000101110"}, {'7', "11111000010001000100010000100001000"}, {'8', "01110100011000101110100011000101110"},
      {'9', "01110100011000101111000010001001100"}, {'A', "01110100011000111111100011000110001"}, {'B', "11110100011000111110100011000111110"},
      {'C', "01110100011000010000100001000101110"}, {'D', "11100100101000110001100011001011100"}, {'E', "11111100001000011110100001000011111"},
      {'F', "11111100001000011110100001000010000"}, {'G', "01110100011000010111100011000101111"}, {'H', "10001100011000111111100011000110001"},
      {'I', "01110001000010000100001000010001110"}, {'J', "00111000100001000010000101001001100"}, {'K', "10001100101010011000101001001010001"},
      {'L', "10000100001000010000100001000011111"}, {'M', "10001110111010110101100011000110001"}, {'N', "10001100011100110101100111000110001"},
      {'O', "01110100011000110001100011000101110"}, {'P', "11110100011000111110100001000010000"}, {'Q', "01110100011000110001101011001001101"},
      {'R', "11110100011000111110101001001010001"}, {'S', "01111100001000001110000010000111110"}, {'T', "11111001000010000100001000010000100"},
      {'U', "10001100011000110001100011000101110"}, {'V', "10001100011000110001100010101000100"}, {'W', "10001100011000110101101011010101010"},
      {'X', "10001100010101000100010101000110001"}, {'Y', "10001100010101000100001000010000100"}, {'Z', "11111000010001000100010001000011111"},
      {'.', "00000000000000000000000000110001100"}, {'-', "00000000000000011111000000000000000"}, {'+', "00000001000010011111001000010000000"},
      {':', "00000001100011000000001100011000000"}, {'/', "00001000010001000100010001000010000"}, {'(', "00010001000100001000010000010000010"},
      {')', "01000001000001000010000100010001000"}, {'_', "00000000000000000000000000000011111"}, {'=', "00000000001111100000111110000000000"},
      {' ', "00000000000000000000000000000000000"}};
  auto it = font.find(c);
  return it == font.end() ? font.at(' ') : it->second;
}

struct Hud {
  RenderScene& scene;
  int width, height;
  // 픽셀(왼쪽 위 원점, y 아래) → 클립 좌표
  float cx(double px) const { return static_cast<float>(2.0 * px / width - 1.0); }
  float cy(double py) const { return static_cast<float>(2.0 * py / height - 1.0); }
  // z 는 0 보다 조금 크게 둔다: 정점 셰이더가 선(법선 0)을 카메라 쪽으로 2e-4 당기므로 0 이면 클립 범위 밖으로 나간다
  RenderVertex v(double px, double py, const std::array<std::uint8_t, 3>& c, float z = 0.01f) const {
    RenderVertex out{};
    out.pos[0] = cx(px), out.pos[1] = cy(py), out.pos[2] = z;
    out.color[0] = c[0], out.color[1] = c[1], out.color[2] = c[2], out.color[3] = 255;
    return out;
  }
  void rect(double x0, double y0, double x1, double y1, const std::array<std::uint8_t, 3>& c, float z = 0.01f) {
    auto& t = scene.hud_triangles;
    t.push_back(v(x0, y0, c, z)), t.push_back(v(x1, y0, c, z)), t.push_back(v(x1, y1, c, z));
    t.push_back(v(x0, y0, c, z)), t.push_back(v(x1, y1, c, z)), t.push_back(v(x0, y1, c, z));
  }
  void line(double x0, double y0, double x1, double y1, const std::array<std::uint8_t, 3>& c) {
    scene.hud_lines.push_back(v(x0, y0, c)), scene.hud_lines.push_back(v(x1, y1, c));
  }
  // 글자: 점 하나가 scale 픽셀. 글자 사이 한 칸.
  double text(double x, double y, const std::string& s, const std::array<std::uint8_t, 3>& c, double scale = 2.0) {
    for (char ch : s) {
      const char* g = glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
      for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
          if (g[row * 5 + col] == '1') rect(x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, c);
      x += 6 * scale;
    }
    return x;
  }
  static double text_width(const std::string& s, double scale = 2.0) { return 6.0 * scale * static_cast<double>(s.size()); }
};

}  // namespace nasa95::render_detail
