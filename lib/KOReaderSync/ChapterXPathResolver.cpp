#include "ChapterXPathResolver.h"

#include <Epub/VisibleTextUtils.h>
#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

// Elements crengine lays out as blocks by default (its html5.css / fb2def.h). crengine drops a
// whitespace-only text run that is the first child of such an element, so text()[N] indices
// below must skip those runs to stay resolvable on the KOReader side.
bool isBlockElement(const std::string& name) {
  static const char* const kBlockTags[] = {
      "html",       "body",   "address", "article",   "aside",  "blockquote", "caption", "center",  "col",
      "colgroup",   "dd",     "details", "dialog",    "dir",    "div",        "dl",      "dt",      "fieldset",
      "figcaption", "figure", "footer",  "form",      "h1",     "h2",         "h3",      "h4",      "h5",
      "h6",         "header", "hgroup",  "hr",        "legend", "li",         "listing", "main",    "menu",
      "nav",        "ol",     "p",       "plaintext", "pre",    "search",     "section", "summary", "table",
      "tbody",      "td",     "tfoot",   "th",        "thead",  "tr",         "ul",      "xmp"};
  for (const char* tag : kBlockTags) {
    if (VisibleTextUtils::equalsTag(name, tag)) {
      return true;
    }
  }
  return false;
}

// Mirrors crengine's IsEmptySpace(): only these four count as empty space.
bool isWhitespaceOnly(const XML_Char* data, const int len) {
  for (int i = 0; i < len; i++) {
    const char c = data[i];
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      return false;
    }
  }
  return true;
}

struct ParentState {
  std::vector<NameCounter> children;
  bool isBlock = false;
  bool childSeen = false;  // an element or kept text node was already added (crengine getChildCount() > 0)

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  // Visible codepoints up to the end of the last non-whitespace run. Trailing formatting
  // whitespace is excluded so a 100% target still resolves inside real text.
  size_t totalVisibleChars() const { return lastTextEndChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
    if (!isWhitespaceOnly(data, len)) {
      lastTextEndChars = visibleChars;
    }
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t lastTextEndChars = 0;
};

class XPathParagraphResolver final : public Print {
 public:
  explicit XPathParagraphResolver(const int targetParagraph) : targetParagraph(targetParagraph) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
  }

  ~XPathParagraphResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    // Count both <p> and <li> as paragraph-like positions, matching how the section
    // layout tracks them (xpathParagraphIndex and xpathListItemIndex). This ensures
    // KOReader progress in list items maps to the correct XPath.
    if (name == "p") {
      paragraphCount++;
    } else if (name == "li") {
      paragraphCount++;
    }
    if (paragraphCount == targetParagraph) {
      xpath = buildParagraphXPath(spineIndex, path, 0, 0);
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetParagraph;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class XPathProgressResolver final : public Print {
 public:
  enum class BoundaryMode { Exclusive, Inclusive };

  explicit XPathProgressResolver(const size_t targetVisibleChar,
                                 const BoundaryMode boundaryMode = BoundaryMode::Exclusive)
      : targetVisibleChar(targetVisibleChar), boundaryMode(boundaryMode) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
    XML_SetCommentHandler(parser, &XPathProgressResolver::comment);
    XML_SetProcessingInstructionHandler(parser, &XPathProgressResolver::processingInstruction);
    XML_SetCdataSectionHandler(parser, &XPathProgressResolver::startCdataSection,
                               &XPathProgressResolver::endCdataSection);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  static void XMLCALL comment(void* userData, const XML_Char*) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL processingInstruction(void* userData, const XML_Char*, const XML_Char*) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL startCdataSection(void* userData) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL endCdataSection(void* userData) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
        parentStates.back().isBlock = true;
      }
      depth++;
      return;
    }

    endTextNode();
    const int siblingIndex = parentStates.back().nextIndex(name);
    parentStates.back().childSeen = true;
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    parentStates.back().isBlock = isBlockElement(name);
    textNodeIndexStack.push_back(0);

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }

    endTextNode();
    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  // Visible-offset counting mirrors ChapterHtmlSlimParser::characterData (every body
  // codepoint outside non-visible elements), so offsets recorded by the page LUT line up
  // with what is counted here regardless of which elements hold the text.
  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0 || stopped) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    const bool whitespaceOnly = isWhitespaceOnly(data, len);

    // A text run starts on the first non-empty content after any structural boundary.
    // Only counting non-empty runs matches KOReader's text()[N] indexing behavior, which
    // skips empty text nodes created by bare <a id="anchor"/> anchors. A run that begins
    // with whitespace as the first child of a block element stays provisional: crengine
    // drops it unless the run goes on to contain non-whitespace text.
    if (pendingTextNode) {
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
      if (whitespaceOnly && parentStates.back().isBlock && !parentStates.back().childSeen) {
        provisionalTextNode = true;
      } else {
        commitTextNode();
      }
    } else if (provisionalTextNode && !whitespaceOnly) {
      commitTextNode();
    }

    // Whitespace-only runs are never used as anchors: a target that falls inside one is
    // carried forward and resolves at the start of the next text run instead.
    const size_t nextVisibleChars = visibleChars + codepointCount;
    const bool targetReached = boundaryMode == BoundaryMode::Inclusive ? targetVisibleChar <= nextVisibleChars
                                                                       : targetVisibleChar < nextVisibleChars;
    if (targetReached && !whitespaceOnly) {
      const size_t delta = targetVisibleChar > visibleChars ? targetVisibleChar - visibleChars : 0;
      const int texNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
      const size_t charOff = visibleChars - textNodeStartChars + delta;
      xpath = buildParagraphXPath(spineIndex, path, texNode, charOff);
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
      return;
    }

    visibleChars = nextVisibleChars;
  }

  void onMarkupBoundary() {
    if (!insideBody || nonVisibleDepth > 0 || stopped) {
      return;
    }

    endTextNode();
  }

  // The current text run becomes a real text node: it takes the next text()[N] index and
  // counts as a child of its parent.
  void commitTextNode() {
    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.back()++;
    }
    parentStates.back().childSeen = true;
    provisionalTextNode = false;
  }

  // A structural boundary ends the current text run. A still-provisional run is dropped,
  // exactly as crengine drops a whitespace-only first child of a block element.
  void endTextNode() {
    provisionalTextNode = false;
    pendingTextNode = true;
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  const BoundaryMode boundaryMode;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  bool provisionalTextNode = false;
  int depth = 0;
  int bodyDepth = -1;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};
}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathParagraphResolver resolver(paragraphIndex);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved paragraph %u in spine %d -> %s", paragraphIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Paragraph %u not found in spine %d", paragraphIndex, spineIndex);
  return "";
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathProgressResolver resolver(visibleTextOffset);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved visible offset %u in spine %d -> %s", visibleTextOffset, spineIndex,
            resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Visible offset %u not found in spine %d", visibleTextOffset, spineIndex);
  return "";
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  XPathProgressResolver resolver(targetVisibleChar, XPathProgressResolver::BoundaryMode::Inclusive);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex,
            resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}
