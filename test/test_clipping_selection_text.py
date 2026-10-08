"""Run with python3 test/test_clipping_selection_text.py (requires a host C/C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/ClipSelectionActivity.cpp").read_text()
header = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text()
word_box = "struct WordBox {" + header.split("struct WordBox {", 1)[1].split("  };", 1)[0] + "};"
clean_word = "const char* cleanWordStart" + source.split("const char* cleanWordStart", 1)[1].split(
    "}  // namespace", 1
)[0]
build_text = "bool ClipSelectionActivity::buildSelectedText" + source.split(
    "bool ClipSelectionActivity::buildSelectedText", 1
)[1].split("void ClipSelectionActivity::confirmSelection", 1)[0]

# Compile the production exporter and BiDi resolver, with only activity/UI dependencies omitted.
harness = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <BidiUtils.h>
#include <Memory.h>
#include <Utf8.h>
#include <Logging.h>
#include "clippings/ClippingText.h"
struct EpdFontFamily { enum Style { REGULAR }; };
constexpr size_t CLIPPING_TEXT_MAX = 4096;
""" + clean_word + r"""
struct ClipSelectionActivity {
""" + word_box + r"""
  std::unique_ptr<WordBox[]> words = makeUniqueNoThrow<WordBox[]>(240);
  bool buildSelectedText(int first, int last, std::string& text) const;
};
""" + build_text + r"""
int loadLine(ClipSelectionActivity& activity, const std::vector<std::string>& logical,
             bool rtl, int first = 0, uint32_t offset = 0, uint16_t row = 0) {
  std::vector<uint16_t> visual;
  if (!BidiUtils::computeVisualWordOrder(logical, rtl, visual)) {
    for (size_t i = 0; i < logical.size(); ++i) visual.push_back(i);
  }
  std::vector<uint32_t> starts;
  for (const auto& text : logical) {
    starts.push_back(offset);
    for (const unsigned char c : text) if ((c & 0xc0) != 0x80) ++offset;
    ++offset;
  }
  for (size_t i = 0; i < visual.size(); ++i) {
    const size_t index = visual[i];
    auto& word = activity.words[first + i];
    word.text = logical[index].c_str();
    word.startOffset = starts[index];
    word.endOffset = index + 1 < starts.size() ? starts[index + 1] - 1 : offset - 1;
    word.x = i * 100;
    word.width = 80;
    word.row = row;
    word.pageOffset = row;
    word.isRtl = rtl;
  }
  // extractWords() keeps screen navigation in paragraph direction.
  if (rtl) std::reverse(activity.words.get() + first, activity.words.get() + first + visual.size());
  return first + visual.size() - 1;
}
void expect(const ClipSelectionActivity& activity, int first, int last, const char* expected) {
  std::string text;
  assert(activity.buildSelectedText(first, last, text));
  if (text != expected) std::fprintf(stderr, "Expected: %s\nActual:   %s\n", expected, text.c_str());
  assert(text == expected);
}
int main() {
  ClipSelectionActivity activity;
  for (const auto& logical : {std::vector<std::string>{"אחד", "alpha", "beta", "שני"},
                              std::vector<std::string>{"واحد", "alpha", "beta", "اثنان"}}) {
    loadLine(activity, logical, true);
    const char* visualSecond = activity.words[1].text;
    expect(activity, 0, 3, (logical[0] + " alpha beta " + logical[3]).c_str());
    expect(activity, 1, 2, "alpha beta");
    expect(activity, 2, 2, "alpha");
    assert(activity.words[1].text == visualSecond);  // Export must not change navigation order.
  }
  const std::vector<std::string> ltr{"alpha", "אחד", "שני", "beta"};
  loadLine(activity, ltr, false);
  expect(activity, 0, 3, "alpha אחד שני beta");
  const std::vector<std::string> firstLine{"אחד", "alpha", "beta", "שני"};
  const std::vector<std::string> secondLine{"واحد", "gamma", "delta", "اثنان"};
  loadLine(activity, firstLine, true);
  loadLine(activity, secondLine, true, 4, 40, 1);
  expect(activity, 0, 7, "אחד alpha beta שני واحد gamma delta اثنان");
  activity.words[4].paragraphStart = true;
  expect(activity, 0, 7, "אחד alpha beta שני\nواحد gamma delta اثنان");
  const std::vector<std::string> plain{"one", "two"};
  loadLine(activity, plain, false);
  activity.words[0].startOffset = activity.words[0].endOffset = UINT32_MAX;
  expect(activity, 0, 1, "one two");
  activity.words[1].startOffset = activity.words[1].endOffset = UINT32_MAX;
  expect(activity, 0, 1, "one two");
  activity.words[0].text = "discre-";
  activity.words[0].startOffset = 0;
  activity.words[0].endOffset = 6;
  activity.words[1].text = "tionary";
  activity.words[1].startOffset = 6;
  activity.words[1].endOffset = 13;
  activity.words[0].discretionaryHyphen = true;
  expect(activity, 0, 1, "discretionary");
  activity.words[0].text = "Café";
  activity.words[0].endOffset = 5;
  activity.words[0].discretionaryHyphen = false;
  activity.words[1].text = "s";
  activity.words[1].startOffset = 5;
  activity.words[1].endOffset = 6;
  expect(activity, 0, 1, "Cafés");
  activity.words[0].text = "Café-";
  activity.words[0].discretionaryHyphen = true;
  expect(activity, 0, 1, "Cafés");
  activity.words[0].endOffset = 6;
  activity.words[0].discretionaryHyphen = false;
  activity.words[1].startOffset = 6;
  activity.words[1].endOffset = 7;
  expect(activity, 0, 1, "Café-s");
  const std::string oversized(CLIPPING_TEXT_MAX + 1, 'x');
  activity.words[0].text = oversized.c_str();
  std::string text;
  assert(!activity.buildSelectedText(0, 0, text));
  std::string expected;
  for (int i = 0; i < 240; ++i) {
    activity.words[i] = {};
    activity.words[i].text = "word";
    activity.words[i].startOffset = (239 - i) * 5;
    activity.words[i].endOffset = activity.words[i].startOffset + 4;
    if (i != 0) expected += ' ';
    expected += "word";
  }
  expect(activity, 0, 239, expected.c_str());
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "selection.cpp"
    executable = Path(directory) / "selection"
    bidi_object = Path(directory) / "minibidi.o"
    cpp.write_text(harness)
    subprocess.run(["cc", "-c", str(ROOT / "lib/MiniBidi/minibidi.c"), "-o", str(bidi_object)], check=True)
    subprocess.run([
        "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "src"), "-I", str(ROOT / "lib/Memory"),
        "-I", str(ROOT / "test/minibidi_arabic/stubs"),
        "-I", str(ROOT / "lib/MiniBidi"), "-I", str(ROOT / "lib/Utf8"),
        str(cpp), str(bidi_object), str(ROOT / "lib/MiniBidi/BidiUtils.cpp"),
        str(ROOT / "lib/Utf8/Utf8.cpp"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Clipping selection text regression checks passed")
