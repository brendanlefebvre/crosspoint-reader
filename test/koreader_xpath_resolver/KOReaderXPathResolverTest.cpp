#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ChapterXPathResolver.h"

namespace {
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

constexpr char kNestedFixture[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><div><section><p>Alpha bravo</p><p>Second <em>nested</em> tail</p></section></div></body></html>)";
constexpr char kNonVisibleInlineFixture[] =
    R"(<html><body><p><RP><span>hidden</span></RP>Visible text</p></body></html>)";
constexpr char kCommentBoundaryFixture[] = R"(<html><body><p>before<!--comment-->after</p></body></html>)";
constexpr char kProcessingInstructionBoundaryFixture[] = R"(<html><body><p>before<?marker?>after</p></body></html>)";
constexpr char kCdataBoundaryFixture[] = R"(<html><body><p>before<![CDATA[middle]]>after</p></body></html>)";
constexpr char kHiddenCdataFixture[] = R"(<html><body><p>before<rp><![CDATA[hidden]]></rp>after</p></body></html>)";
}  // namespace

TEST(KOReaderXPathResolver, ResolvesExactOffsetWithFullAncestry) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNestedFixture), 0, 6),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6");
}

TEST(KOReaderXPathResolver, PreservesNestedInlineTextNode) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNestedFixture), 0, 26),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].2");
}

TEST(KOReaderXPathResolver, EmitsDetailedAnchorForOffsetZero) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNestedFixture), 0, 0),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, IgnoresNestedNonVisibleInlineText) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNonVisibleInlineFixture), 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, ResolvesProgressAfterNestedNonVisibleInlineText) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epubWith(kNonVisibleInlineFixture), 0, 1.0f),
            "/body/DocFragment[1]/body/p[1]/text()[1].12");
}

TEST(KOReaderXPathResolver, CountsUtf8CodepointsInsteadOfBytes) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html><body><p>A\xC3\xA9\xE4\xB8\xAD"
      "B</p></body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundComments) {
  const auto epub = epubWith(kCommentBoundaryFixture);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/p[1]/text()[2].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 11).empty());
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundProcessingInstructions) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kProcessingInstructionBoundaryFixture), 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundCdata) {
  const auto epub = epubWith(kCdataBoundaryFixture);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 13),
            "/body/DocFragment[1]/body/p[1]/text()[3].1");
}

TEST(KOReaderXPathResolver, CountsVisibleCdataAndIgnoresHiddenCdata) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kHiddenCdataFixture), 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, PreservesDirectBodyFallback) {
  EXPECT_EQ(
      ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body>Direct text</body></html>"), 0, 0),
      "/body/DocFragment[1]/body");
}

TEST(KOReaderXPathResolver, ReturnsEmptyForUnusableContent) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(""), 0, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body><p>broken"), 0, 100).empty());
}

TEST(KOReaderXPathResolver, KeepsParagraphOnlyResolutionUnchanged) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epubWith(kNestedFixture), 0, 2),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]");
}

namespace {
// Inky keeps formatting whitespace around a split chapter's mapped children.
constexpr char kSplitFixture[] =
    "<html><body>\n\n <section>\n\n <p>Alpha</p>\n\n <p>Bravo</p>\n\n </section>\n</body></html>";
}  // namespace

TEST(KOReaderXPathResolver, SplitStartSkipsBodyAndContainerWhitespace) {
  const auto epub = epubWith(kSplitFixture);
  for (uint32_t offset = 0; offset <= 6; ++offset) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset, 2),
              "/body/DocFragment[1]/body/section[1]/p[1]/text()[1].0")
        << offset;
  }
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.0f, 2),
            "/body/DocFragment[1]/body/section[1]/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, SplitInteriorRetainsExactOffset) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kSplitFixture), 0, 8, 2),
            "/body/DocFragment[1]/body/section[1]/p[1]/text()[1].2");
}

TEST(KOReaderXPathResolver, SplitGapAdvancesToNextChild) {
  const auto epub = epubWith(kSplitFixture);
  for (uint32_t offset = 11; offset <= 14; ++offset) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset, 2),
              "/body/DocFragment[1]/body/section[1]/p[2]/text()[1].0")
        << offset;
  }
}

TEST(KOReaderXPathResolver, SplitEndUsesLastContentCharacter) {
  const auto epub = epubWith(kSplitFixture);
  for (uint32_t offset = 19; offset <= 23; ++offset) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset, 2),
              "/body/DocFragment[1]/body/section[1]/p[2]/text()[1].4")
        << offset;
  }
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f, 2),
            "/body/DocFragment[1]/body/section[1]/p[2]/text()[1].4");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 100, 2).empty());
}

TEST(KOReaderXPathResolver, SplitWithoutMappedContentDoesNotInventAnchor) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(
                  epubWith("<html><body>\n<section>\n</section></body></html>"), 0, 0, 2)
                  .empty());
}

TEST(KOReaderXPathResolver, SplitDirectChildrenAndNestedInlineText) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(
                epubWith("<html><body>\n<p><em>Hello</em></p></body></html>"), 0, 0, 1),
            "/body/DocFragment[1]/body/p[1]/em[1]/text()[1].0");
}

namespace {
// Calibre output from Kindle/MOBI sources may carry body text in <div>/<span> with no <p>
// beyond an empty filepos anchor (upstream crosspoint-reader#3923, issue #3665).
// Visible offsets below follow the layout parser: every body codepoint counts, including
// formatting whitespace between blocks.
//   0 "\n"   1 "\n"   2 "3"   3 " "   4 "\n"   5-13 "She drove"   14-26 " back to D.C."
//   27 "\n"  28-33 "“Who?”"   34 "\n"
constexpr char kKindleDivSpanFixture[] =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body class=\"calibre\">\n"
    "<p id=\"filepos1\" class=\"calibre1\"></p>\n"
    "<div class=\"calibre8\"><span class=\"calibre9\">3</span></div><div class=\"calibre10\"> </div>\n"
    "<div class=\"calibre12\"><span class=\"calibre6\"><span class=\"bold\">She drove</span> back to "
    "D.C.</span></div>\n"
    "<div class=\"calibre19\"><span class=\"calibre6\">\xE2\x80\x9CWho?\xE2\x80\x9D</span></div>\n"
    "</body></html>";
}  // namespace

TEST(KOReaderXPathResolver, ResolvesTextOutsideParagraphElements) {
  const auto epub = epubWith(kKindleDivSpanFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 2),
            "/body/DocFragment[1]/body/div[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 5),
            "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 20),
            "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].6");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 28),
            "/body/DocFragment[1]/body/div[4]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(
                epubWith("<html><body><div>not a paragraph or list item</div></body></html>"), 0, 0),
            "/body/DocFragment[1]/body/div[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, ResolvesProgressOutsideParagraphElements) {
  const auto epub = epubWith(kKindleDivSpanFixture);

  // Progress spans the text up to the last non-whitespace codepoint (34), so 1.0 lands at the
  // end of the final text node rather than in the trailing formatting newline.
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f),
            "/body/DocFragment[1]/body/div[4]/span[1]/text()[1].6");
  // ceil(0.5 * 34) = 17 -> " back to D.C." starts at 14 -> offset 3 within the node.
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f),
            "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].3");
}

TEST(KOReaderXPathResolver, SkipsWhitespaceOnlyTextWhenResolvingOffsets) {
  const auto epub = epubWith(kKindleDivSpanFixture);

  // Formatting whitespace between blocks is never a usable anchor: resolve to the next text.
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 1),
            "/body/DocFragment[1]/body/div[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 4),
            "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
  // Trailing whitespace with no text after it cannot be resolved.
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 34).empty());
}

TEST(KOReaderXPathResolver, ProgressEndExcludesTrailingWhitespaceInsideRun) {
  const auto epub = epubWith("<html><body><p>Text   </p></body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[1]/text()[1].4");
}

TEST(KOReaderXPathResolver, NumbersTextNodesAfterInlineElementsOutsideParagraphs) {
  // crengine numbers each text child of the <div>: "one " is text()[1], " three" is text()[2].
  //   0-3 "one "   4-6 "two"   7-12 " three"
  const auto epub = epubWith("<html><body><div>one <i>two</i> three</div></body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 9),
            "/body/DocFragment[1]/body/div[1]/text()[2].2");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 5),
            "/body/DocFragment[1]/body/div[1]/i[1]/text()[1].1");
}
