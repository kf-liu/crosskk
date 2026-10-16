#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace WeReadXhtmlCodec {

bool writeLiteral(HalFile& output, const char* text);
bool writeXmlText(HalFile& output, const char* text);

bool sanitizeChapter(const std::string& inputPath, const std::string& outputPath, const std::string& imageIndexPath,
                     uint32_t chapterIndex, const char* title, bool plainText, uint8_t* readBuffer,
                     size_t readBufferSize, char* tagBuffer, size_t tagBufferSize);

bool visibleToNativeOffset(const std::string& path, uint32_t visibleOffset, uint32_t& nativeOffset);
bool nativeToVisibleOffset(const std::string& path, uint32_t nativeOffset, uint32_t& visibleOffset);
// One pass over the chapter. Unmapped entries stay UINT32_MAX. nativeOffsets is not reordered.
bool nativeOffsetsToVisible(const std::string& path, const uint32_t* nativeOffsets, uint32_t* visibleOffsets,
                            size_t count);

// Page words omit the ASCII whitespace the reader still counts in visible offsets.
// Walks the chapter once. nextWord returns the next rendered word, or nullptr at
// the end of the page. onWord reports that word's visible start and length.
// Returns false when the chapter cannot be read. A later mismatch stops the walk;
// words already reported stay aligned.
bool alignRenderedWords(const std::string& path, uint32_t pageVisibleStart, const char* (*nextWord)(void* context),
                        void (*onWord)(uint32_t start, uint16_t span, void* context), void* context);

// Copies the visible text in [start, end). ASCII space, tab, and newlines are
// skipped: the reader counts them, but they are not part of the highlighted words.
// Returns false when the chapter cannot be read. written is the UTF-8 byte count.
bool copyVisibleText(const std::string& path, uint32_t start, uint32_t end, char* dest, size_t destSize,
                     size_t& written);

}  // namespace WeReadXhtmlCodec
