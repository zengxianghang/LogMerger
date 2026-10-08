#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <cwchar>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kNsPerSecond = 1000000000LL;
constexpr int64_t kGpsWeekSeconds = 604800LL;
constexpr int64_t kGpsWeekNs = kGpsWeekSeconds * kNsPerSecond;
constexpr size_t kIoBufferSize = 4U * 1024U * 1024U;
constexpr uint64_t kProgressStepBytes = 1ULL << 30;  // 1 GiB

struct FileCloser {
    void operator()(FILE *fp) const {
        if (fp != nullptr) {
            std::fclose(fp);
        }
    }
};

struct LineReader {
    FILE *fp = nullptr;
    uint64_t bytesRead = 0;
    uint64_t lineNumber = 0;

    bool Read(std::string &text, std::string &eol) {
        text.clear();
        eol.clear();

        char buffer[64 * 1024];
        bool gotAny = false;

        for (;;) {
            if (std::fgets(buffer, static_cast<int>(sizeof(buffer)), fp) == nullptr) {
                if (std::ferror(fp)) {
                    return false;
                }
                if (gotAny) {
                    // Count a final physical line even when the file does not
                    // end with CR/LF. This is common for a truncated log line.
                    ++lineNumber;
                }
                return gotAny;
            }

            gotAny = true;
            const size_t n = std::strlen(buffer);
            bytesRead += static_cast<uint64_t>(n);

            if (n > 0 && buffer[n - 1] == '\n') {
                if (n >= 2 && buffer[n - 2] == '\r') {
                    text.append(buffer, n - 2);
                    eol = "\r\n";
                } else {
                    text.append(buffer, n - 1);
                    eol = "\n";
                }
                ++lineNumber;
                return true;
            }

            text.append(buffer, n);
        }
    }
};

struct ParsedTime {
    int64_t gpstNs = 0;
};


enum class AuxRecordKind {
    kInspva = 0,
    kAux
};

enum class TargetLogType {
    kNone = 0,
    kRange,
    kBestPos,
    kBestVel,
    kPsrVel,
    kPsrPos
};

enum class AuxLogType {
    kNone = 0,

    // NovAtel OEM7 long ASCII logs.
    kNovatelGloEphemeris,
    kNovatelQzssEphemeris,
    kNovatelGalEphemeris,
    kNovatelGpsEphem,
    kNovatelGpsL1cEphem,
    kNovatelBd2Ephem,
    kNovatelIonUtc,
    kNovatelBd2IonUtc,

    // Unicore N4 long ASCII logs.
    kUnicoreGpsIon,
    kUnicoreBd3Ion,
    kUnicoreBdsIon,
    kUnicoreGalIon,
    kUnicoreGpsEph,
    kUnicoreGpsCnavEph,
    kUnicoreQzssEph,
    kUnicoreBd3Eph,
    kUnicoreBdsEph,
    kUnicoreGloEph,
    kUnicoreGalEph,
    kUnicoreIrnssEph
};

struct TimedAuxLine {
    std::string text;
    int64_t gpstNs = 0;
    AuxRecordKind kind = AuxRecordKind::kInspva;
};

struct Stats {
    uint64_t inputLines = 0;
    uint64_t targetLines = 0;
    uint64_t validTargetHeaders = 0;
    uint64_t rangeLines = 0;
    uint64_t bestPosLines = 0;
    uint64_t bestVelLines = 0;
    uint64_t psrVelLines = 0;
    uint64_t psrPosLines = 0;
    uint64_t inspvaLines = 0;
    uint64_t insertedInspvaLines = 0;
    uint64_t unmatchedInspvaBeforeTarget = 0;
    uint64_t unmatchedInspvaAfterInput = 0;
    uint64_t auxLines = 0;
    uint64_t insertedAuxLines = 0;
    uint64_t unmatchedAuxBeforeTarget = 0;
    uint64_t unmatchedAuxAfterInput = 0;
    uint64_t ignoredAuxLines = 0;

    uint64_t novatelGloEphemerisLines = 0;
    uint64_t novatelQzssEphemerisLines = 0;
    uint64_t novatelGalEphemerisLines = 0;
    uint64_t novatelGpsEphemLines = 0;
    uint64_t novatelGpsL1cEphemLines = 0;
    uint64_t novatelBd2EphemLines = 0;
    uint64_t novatelIonUtcLines = 0;
    uint64_t novatelBd2IonUtcLines = 0;

    uint64_t unicoreGpsIonLines = 0;
    uint64_t unicoreBd3IonLines = 0;
    uint64_t unicoreBdsIonLines = 0;
    uint64_t unicoreGalIonLines = 0;
    uint64_t unicoreGpsEphLines = 0;
    uint64_t unicoreGpsCnavEphLines = 0;
    uint64_t unicoreQzssEphLines = 0;
    uint64_t unicoreBd3EphLines = 0;
    uint64_t unicoreBdsEphLines = 0;
    uint64_t unicoreGloEphLines = 0;
    uint64_t unicoreGalEphLines = 0;
    uint64_t unicoreIrnssEphLines = 0;
};

std::string_view TrimAscii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

uint32_t Crc32Value(uint32_t value) {
    for (int bit = 0; bit < 8; ++bit) {
        if ((value & 1U) != 0U) {
            value = (value >> 1U) ^ 0xEDB88320U;
        } else {
            value >>= 1U;
        }
    }
    return value;
}

uint32_t CalculateLongAsciiCrc32(std::string_view data) {
    uint32_t crc = 0U;
    for (const char c : data) {
        const uint32_t byte = static_cast<unsigned char>(c);
        const uint32_t temp1 = (crc >> 8U) & 0x00FFFFFFU;
        const uint32_t temp2 = Crc32Value((crc ^ byte) & 0xFFU);
        crc = temp1 ^ temp2;
    }
    return crc;
}

bool ParseHexUint32(std::string_view text, uint32_t &value) {
    text = TrimAscii(text);
    if (text.size() != 8U) {
        return false;
    }

    uint32_t result = 0U;
    for (const char c : text) {
        uint32_t digit = 0U;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        result = (result << 4U) | digit;
    }

    value = result;
    return true;
}

bool HasValidLongAsciiCrc(std::string_view line) {
    line = TrimAscii(line);
    if (line.size() < 10U || (line.front() != '#' && line.front() != '%')) {
        return false;
    }

    const size_t asterisk = line.rfind('*');
    if (asterisk == std::string_view::npos || asterisk <= 1U) {
        return false;
    }

    uint32_t expectedCrc = 0U;
    if (!ParseHexUint32(line.substr(asterisk + 1U), expectedCrc)) {
        return false;
    }

    // NovAtel ASCII CRC covers every byte after '#'/'%' and before '*'.
    const std::string_view crcData = line.substr(1U, asterisk - 1U);
    return CalculateLongAsciiCrc32(crcData) == expectedCrc;
}

bool GetMessageName(std::string_view line, std::string_view &name) {
    line = TrimAscii(line);
    if (line.empty()) {
        return false;
    }

    // NovAtel long ASCII normally begins with '#'. '%' is retained for
    // compatibility with logs that use a custom textual prefix convention.
    if (line.front() == '#' || line.front() == '%') {
        line.remove_prefix(1);
    }

    size_t end = 0;
    while (end < line.size() && line[end] != ',' && line[end] != ';' &&
           line[end] != ' ' && line[end] != '\t') {
        ++end;
    }

    if (end == 0) {
        return false;
    }
    name = line.substr(0, end);
    return true;
}

TargetLogType ClassifyTargetLine(std::string_view line) {
    std::string_view name;
    if (!GetMessageName(line, name)) {
        return TargetLogType::kNone;
    }

    // Exact matches only: e.g. RANGE2A and MPSRPOSA are intentionally ignored.
    if (name == "RANGEA" || name == "RANGE") {
        return TargetLogType::kRange;
    }
    if (name == "BESTPOSA" || name == "BESTPOS") {
        return TargetLogType::kBestPos;
    }
    if (name == "BESTVELA" || name == "BESTVEL") {
        return TargetLogType::kBestVel;
    }
    if (name == "PSRVELA" || name == "PSRVEL") {
        return TargetLogType::kPsrVel;
    }
    if (name == "PSRPOSA" || name == "PSRPOS") {
        return TargetLogType::kPsrPos;
    }
    return TargetLogType::kNone;
}

bool IsInspvaLine(std::string_view line) {
    std::string_view name;
    if (!GetMessageName(line, name)) {
        return false;
    }
    // NovAtel long ASCII INSPVA is normally named INSPVAA.
    return name == "INSPVAA" || name == "INSPVA";
}

AuxLogType ClassifyAuxLine(std::string_view line) {
    std::string_view name;
    if (!GetMessageName(line, name)) {
        return AuxLogType::kNone;
    }

    // NovAtel OEM7 names. Exact matching prevents accidental acceptance of
    // similarly named proprietary logs.
    if (name == "GLOEPHEMERISA" || name == "GLOEPHEMERIS") {
        return AuxLogType::kNovatelGloEphemeris;
    }
    if (name == "QZSSEPHEMERISA" || name == "QZSSEPHEMERIS") {
        return AuxLogType::kNovatelQzssEphemeris;
    }
    if (name == "GALEPHEMERISA" || name == "GALEPHEMERIS") {
        return AuxLogType::kNovatelGalEphemeris;
    }
    if (name == "GPSEPHEMA" || name == "GPSEPHEM") {
        return AuxLogType::kNovatelGpsEphem;
    }
    if (name == "GPSL1CEPHEMA" || name == "GPSL1CEPHEM") {
        return AuxLogType::kNovatelGpsL1cEphem;
    }
    if (name == "BD2EPHEMA" || name == "BD2EPHEM") {
        return AuxLogType::kNovatelBd2Ephem;
    }
    if (name == "IONUTCA" || name == "IONUTC") {
        return AuxLogType::kNovatelIonUtc;
    }
    if (name == "BD2IONUTCA" || name == "BD2IONUTC") {
        return AuxLogType::kNovatelBd2IonUtc;
    }

    // Unicore N4 ASCII output names. The manual's ASCII syntax appends A to
    // the base message name; base names are also accepted for normalized text
    // exports.
    if (name == "GPSIONA" || name == "GPSION") {
        return AuxLogType::kUnicoreGpsIon;
    }
    if (name == "BD3IONA" || name == "BD3ION") {
        return AuxLogType::kUnicoreBd3Ion;
    }
    if (name == "BDSIONA" || name == "BDSION") {
        return AuxLogType::kUnicoreBdsIon;
    }
    if (name == "GALIONA" || name == "GALION") {
        return AuxLogType::kUnicoreGalIon;
    }
    if (name == "GPSEPHA" || name == "GPSEPH") {
        return AuxLogType::kUnicoreGpsEph;
    }
    if (name == "GPSCNAVEPHA" || name == "GPSCNAVEPH") {
        return AuxLogType::kUnicoreGpsCnavEph;
    }
    if (name == "QZSSEPHA" || name == "QZSSEPH") {
        return AuxLogType::kUnicoreQzssEph;
    }
    if (name == "BD3EPHA" || name == "BD3EPH") {
        return AuxLogType::kUnicoreBd3Eph;
    }
    if (name == "BDSEPHA" || name == "BDSEPH") {
        return AuxLogType::kUnicoreBdsEph;
    }
    if (name == "GLOEPHA" || name == "GLOEPH") {
        return AuxLogType::kUnicoreGloEph;
    }
    if (name == "GALEPHA" || name == "GALEPH") {
        return AuxLogType::kUnicoreGalEph;
    }
    if (name == "IRNSSEPHA" || name == "IRNSSEPH") {
        return AuxLogType::kUnicoreIrnssEph;
    }
    return AuxLogType::kNone;
}

bool IsUnicoreAuxLog(AuxLogType type) {
    switch (type) {
        case AuxLogType::kUnicoreGpsIon:
        case AuxLogType::kUnicoreBd3Ion:
        case AuxLogType::kUnicoreBdsIon:
        case AuxLogType::kUnicoreGalIon:
        case AuxLogType::kUnicoreGpsEph:
        case AuxLogType::kUnicoreGpsCnavEph:
        case AuxLogType::kUnicoreQzssEph:
        case AuxLogType::kUnicoreBd3Eph:
        case AuxLogType::kUnicoreBdsEph:
        case AuxLogType::kUnicoreGloEph:
        case AuxLogType::kUnicoreGalEph:
        case AuxLogType::kUnicoreIrnssEph:
            return true;
        default:
            return false;
    }
}

void CountAuxLine(AuxLogType type, Stats &stats) {
    ++stats.auxLines;
    switch (type) {
        case AuxLogType::kNovatelGloEphemeris: ++stats.novatelGloEphemerisLines; break;
        case AuxLogType::kNovatelQzssEphemeris: ++stats.novatelQzssEphemerisLines; break;
        case AuxLogType::kNovatelGalEphemeris: ++stats.novatelGalEphemerisLines; break;
        case AuxLogType::kNovatelGpsEphem: ++stats.novatelGpsEphemLines; break;
        case AuxLogType::kNovatelGpsL1cEphem: ++stats.novatelGpsL1cEphemLines; break;
        case AuxLogType::kNovatelBd2Ephem: ++stats.novatelBd2EphemLines; break;
        case AuxLogType::kNovatelIonUtc: ++stats.novatelIonUtcLines; break;
        case AuxLogType::kNovatelBd2IonUtc: ++stats.novatelBd2IonUtcLines; break;
        case AuxLogType::kUnicoreGpsIon: ++stats.unicoreGpsIonLines; break;
        case AuxLogType::kUnicoreBd3Ion: ++stats.unicoreBd3IonLines; break;
        case AuxLogType::kUnicoreBdsIon: ++stats.unicoreBdsIonLines; break;
        case AuxLogType::kUnicoreGalIon: ++stats.unicoreGalIonLines; break;
        case AuxLogType::kUnicoreGpsEph: ++stats.unicoreGpsEphLines; break;
        case AuxLogType::kUnicoreGpsCnavEph: ++stats.unicoreGpsCnavEphLines; break;
        case AuxLogType::kUnicoreQzssEph: ++stats.unicoreQzssEphLines; break;
        case AuxLogType::kUnicoreBd3Eph: ++stats.unicoreBd3EphLines; break;
        case AuxLogType::kUnicoreBdsEph: ++stats.unicoreBdsEphLines; break;
        case AuxLogType::kUnicoreGloEph: ++stats.unicoreGloEphLines; break;
        case AuxLogType::kUnicoreGalEph: ++stats.unicoreGalEphLines; break;
        case AuxLogType::kUnicoreIrnssEph: ++stats.unicoreIrnssEphLines; break;
        default: break;
    }
}

const char *TargetLogName(TargetLogType type) {
    switch (type) {
        case TargetLogType::kRange:
            return "RANGE";
        case TargetLogType::kBestPos:
            return "BESTPOS";
        case TargetLogType::kBestVel:
            return "BESTVEL";
        case TargetLogType::kPsrVel:
            return "PSRVEL";
        case TargetLogType::kPsrPos:
            return "PSRPOS";
        default:
            return "UNKNOWN";
    }
}

void CountTargetLine(TargetLogType type, Stats &stats) {
    ++stats.targetLines;
    switch (type) {
        case TargetLogType::kRange:
            ++stats.rangeLines;
            break;
        case TargetLogType::kBestPos:
            ++stats.bestPosLines;
            break;
        case TargetLogType::kBestVel:
            ++stats.bestVelLines;
            break;
        case TargetLogType::kPsrVel:
            ++stats.psrVelLines;
            break;
        case TargetLogType::kPsrPos:
            ++stats.psrPosLines;
            break;
        default:
            break;
    }
}

bool GetCsvFieldBeforeSemicolon(std::string_view line, int wantedIndex,
                                std::string_view &field) {
    const size_t semicolon = line.find(';');
    const std::string_view header =
        (semicolon == std::string_view::npos) ? line : line.substr(0, semicolon);

    int index = 0;
    size_t begin = 0;
    while (begin <= header.size()) {
        const size_t comma = header.find(',', begin);
        const size_t end =
            (comma == std::string_view::npos) ? header.size() : comma;

        if (index == wantedIndex) {
            field = TrimAscii(header.substr(begin, end - begin));
            return true;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
        ++index;
    }
    return false;
}

bool ParseNonNegativeInt64(std::string_view s, int64_t &value) {
    s = TrimAscii(s);
    if (s.empty()) {
        return false;
    }

    size_t pos = 0;
    if (s[pos] == '+') {
        ++pos;
    }
    if (pos >= s.size()) {
        return false;
    }

    int64_t result = 0;
    bool hasDigit = false;
    for (; pos < s.size(); ++pos) {
        const char c = s[pos];
        if (c < '0' || c > '9') {
            return false;
        }
        hasDigit = true;
        const int digit = c - '0';
        if (result > (std::numeric_limits<int64_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }

    if (!hasDigit) {
        return false;
    }
    value = result;
    return true;
}

bool ParseSecondsToNs(std::string_view s, int64_t &ns) {
    s = TrimAscii(s);
    if (s.empty()) {
        return false;
    }

    size_t pos = 0;
    if (s[pos] == '+') {
        ++pos;
    }
    if (pos >= s.size()) {
        return false;
    }

    int64_t integerPart = 0;
    bool hasIntegerDigit = false;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        hasIntegerDigit = true;
        const int digit = s[pos] - '0';
        if (integerPart > (std::numeric_limits<int64_t>::max() - digit) / 10) {
            return false;
        }
        integerPart = integerPart * 10 + digit;
        ++pos;
    }

    if (!hasIntegerDigit) {
        return false;
    }

    int64_t fractionNs = 0;
    int fractionDigits = 0;
    int roundingDigit = -1;

    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            const int digit = s[pos] - '0';
            if (fractionDigits < 9) {
                fractionNs = fractionNs * 10 + digit;
                ++fractionDigits;
            } else if (roundingDigit < 0) {
                roundingDigit = digit;
            }
            ++pos;
        }
    }

    if (pos != s.size()) {
        return false;
    }

    while (fractionDigits < 9) {
        fractionNs *= 10;
        ++fractionDigits;
    }

    if (roundingDigit >= 5) {
        ++fractionNs;
        if (fractionNs == kNsPerSecond) {
            fractionNs = 0;
            ++integerPart;
        }
    }

    if (integerPart > (std::numeric_limits<int64_t>::max() - fractionNs) /
                          kNsPerSecond) {
        return false;
    }
    ns = integerPart * kNsPerSecond + fractionNs;
    return true;
}

bool ParseNovatelGpst(std::string_view line, ParsedTime &parsed,
                      std::string &error) {
    // Common NovAtel OEM7 long ASCII header used by RANGE, BESTPOS,
    // BESTVEL, PSRVEL, PSRPOS and INSPVA (0-based fields):
    // 0 message, 1 port, 2 sequence, 3 idle, 4 time status,
    // 5 GPS week, 6 GPS seconds-of-week, ...
    std::string_view weekField;
    std::string_view sowField;
    if (!GetCsvFieldBeforeSemicolon(line, 5, weekField) ||
        !GetCsvFieldBeforeSemicolon(line, 6, sowField)) {
        error = "header has fewer than 7 comma-separated fields";
        return false;
    }

    int64_t week = 0;
    int64_t sowNs = 0;
    if (!ParseNonNegativeInt64(weekField, week)) {
        error = "invalid GPS week field";
        return false;
    }
    if (!ParseSecondsToNs(sowField, sowNs)) {
        error = "invalid GPS seconds-of-week field";
        return false;
    }

    if (sowNs < 0 || sowNs > kGpsWeekNs) {
        error = "GPS seconds-of-week is outside [0, 604800]";
        return false;
    }

    // Normalize exactly 604800.0 to the next GPS week.
    if (sowNs == kGpsWeekNs) {
        ++week;
        sowNs = 0;
    }

    if (week > (std::numeric_limits<int64_t>::max() - sowNs) / kGpsWeekNs) {
        error = "GPS time overflows int64 nanoseconds";
        return false;
    }

    parsed.gpstNs = week * kGpsWeekNs + sowNs;
    return true;
}

bool ParseUnicoreGpst(std::string_view line, ParsedTime &parsed,
                      std::string &error) {
    // Unicore N4 long ASCII header (0-based fields):
    // 0 message, 1 CPU idle, 2 time reference, 3 time status,
    // 4 GPS week, 5 GPS milliseconds-of-week, 6 version, ...
    // The N4 R1.15 ASCII header table explicitly defines Wn as GPS week and
    // Ms as GPS milliseconds-of-week, so no BDST epoch conversion is applied.
    std::string_view weekField;
    std::string_view msField;
    if (!GetCsvFieldBeforeSemicolon(line, 4, weekField) ||
        !GetCsvFieldBeforeSemicolon(line, 5, msField)) {
        error = "Unicore header has fewer than 6 comma-separated fields";
        return false;
    }

    int64_t week = 0;
    int64_t ms = 0;
    if (!ParseNonNegativeInt64(weekField, week)) {
        error = "invalid Unicore GPS week field";
        return false;
    }
    if (!ParseNonNegativeInt64(msField, ms)) {
        error = "invalid Unicore GPS milliseconds-of-week field";
        return false;
    }

    constexpr int64_t kMsPerGpsWeek = kGpsWeekSeconds * 1000LL;
    if (ms < 0 || ms > kMsPerGpsWeek) {
        error = "Unicore GPS milliseconds-of-week is outside [0, 604800000]";
        return false;
    }

    if (ms == kMsPerGpsWeek) {
        ++week;
        ms = 0;
    }

    const int64_t sowNs = ms * 1000000LL;
    if (week > (std::numeric_limits<int64_t>::max() - sowNs) / kGpsWeekNs) {
        error = "Unicore GPS time overflows int64 nanoseconds";
        return false;
    }

    parsed.gpstNs = week * kGpsWeekNs + sowNs;
    return true;
}

bool WriteBytes(FILE *fp, const char *data, size_t size) {
    return size == 0 || std::fwrite(data, 1, size, fp) == size;
}

bool WriteLine(FILE *fp, const std::string &text, const std::string &eol) {
    return WriteBytes(fp, text.data(), text.size()) &&
           WriteBytes(fp, eol.data(), eol.size());
}

FILE *OpenReadBinary(const fs::path &path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

FILE *OpenWriteBinary(const fs::path &path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

std::string PathForMessage(const fs::path &path) {
#ifdef _WIN32
    const std::wstring ws = path.wstring();
    std::string result;
    result.reserve(ws.size());
    for (wchar_t c : ws) {
        result.push_back(c <= 0x7F ? static_cast<char>(c) : '?');
    }
    return result;
#else
    return path.string();
#endif
}

bool PathsLikelyEqual(const fs::path &a, const fs::path &b) {
    std::error_code ec1;
    std::error_code ec2;
    fs::path aa = fs::absolute(a, ec1).lexically_normal();
    fs::path bb = fs::absolute(b, ec2).lexically_normal();
    if (ec1 || ec2) {
        return false;
    }
#ifdef _WIN32
    std::wstring as = aa.wstring();
    std::wstring bs = bb.wstring();
    for (wchar_t &c : as) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    for (wchar_t &c : bs) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return as == bs;
#else
    return aa == bb;
#endif
}

bool ReadNextAuxRecord(LineReader &reader, TimedAuxLine &out, bool &hasLine,
                   int64_t &lastGpstNs, bool &hasLastGpst, Stats &stats,
                   std::string &error) {
    std::string text;
    std::string eol;

    while (reader.Read(text, eol)) {
        const bool isInspva = IsInspvaLine(text);
        const AuxLogType auxType = isInspva ? AuxLogType::kNone
                                             : ClassifyAuxLine(text);
        if (!isInspva && auxType == AuxLogType::kNone) {
            ++stats.ignoredAuxLines;
            continue;
        }

        // Never trust a time header from a truncated/corrupted source record.
        if (!HasValidLongAsciiCrc(text)) {
            continue;
        }

        ParsedTime parsed;
        std::string parseError;
        bool parsedOk = false;
        if (isInspva) {
            parsedOk = ParseNovatelGpst(text, parsed, parseError);
        } else {
            parsedOk = IsUnicoreAuxLog(auxType)
                           ? ParseUnicoreGpst(text, parsed, parseError)
                           : ParseNovatelGpst(text, parsed, parseError);
        }
        if (!parsedOk) {
            continue;
        }

        // In mixed mode all eligible records share one chronological stream.
        // This both preserves source order at equal GPST and keeps the merge
        // one-pass for multi-GB files.
        if (hasLastGpst && parsed.gpstNs < lastGpstNs) {
            error = "mixed input GPST is not nondecreasing at line " +
                    std::to_string(reader.lineNumber);
            return false;
        }

        lastGpstNs = parsed.gpstNs;
        hasLastGpst = true;
        out.text = std::move(text);
        out.gpstNs = parsed.gpstNs;
        out.kind = isInspva ? AuxRecordKind::kInspva : AuxRecordKind::kAux;
        hasLine = true;

        if (isInspva) {
            ++stats.inspvaLines;
        } else {
            CountAuxLine(auxType, stats);
        }
        return true;
    }

    if (std::ferror(reader.fp)) {
        error = "failed while reading mixed input";
        return false;
    }

    hasLine = false;
    return true;
}

int64_t ParseToleranceUs(const fs::path &arg, bool &ok) {
#ifdef _WIN32
    const std::wstring s = arg.wstring();
    wchar_t *end = nullptr;
    errno = 0;
    const long long value = std::wcstoll(s.c_str(), &end, 10);
    ok = (errno == 0 && end != s.c_str() && *end == L'\0' && value >= 0 &&
          value <= 1000000LL);
#else
    const std::string s = arg.string();
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(s.c_str(), &end, 10);
    ok = (errno == 0 && end != s.c_str() && *end == '\0' && value >= 0 &&
          value <= 1000000LL);
#endif
    if (!ok) {
        return 0;
    }
    return static_cast<int64_t>(value) * 1000LL;
}

int Run(const fs::path &inputPath, const fs::path &auxPath,
        const fs::path &outputPath, int64_t toleranceNs) {
    if (PathsLikelyEqual(inputPath, outputPath) ||
        PathsLikelyEqual(auxPath, outputPath) ||
        PathsLikelyEqual(inputPath, auxPath)) {
        std::fprintf(stderr,
                     "ERROR: input.log, aux.log and output.log must be different files.\n");
        return 2;
    }

    fs::path tempOutputPath = outputPath;
    tempOutputPath += ".part";

    std::error_code ec;
    fs::remove(tempOutputPath, ec);

    FILE *input = OpenReadBinary(inputPath);
    if (input == nullptr) {
        std::fprintf(stderr, "ERROR: cannot open input log: %s\n",
                     PathForMessage(inputPath).c_str());
        return 3;
    }

    FILE *aux = OpenReadBinary(auxPath);
    if (aux == nullptr) {
        std::fprintf(stderr, "ERROR: cannot open auxiliary log: %s\n",
                     PathForMessage(auxPath).c_str());
        std::fclose(input);
        return 3;
    }

    FILE *output = OpenWriteBinary(tempOutputPath);
    if (output == nullptr) {
        std::fprintf(stderr, "ERROR: cannot create temporary output: %s\n",
                     PathForMessage(tempOutputPath).c_str());
        std::fclose(input);
        std::fclose(aux);
        return 3;
    }

    char *inputBuffer = new (std::nothrow) char[kIoBufferSize];
    char *auxBuffer = new (std::nothrow) char[kIoBufferSize];
    char *outputBuffer = new (std::nothrow) char[kIoBufferSize];
    if (inputBuffer != nullptr) {
        std::setvbuf(input, inputBuffer, _IOFBF, kIoBufferSize);
    }
    if (auxBuffer != nullptr) {
        std::setvbuf(aux, auxBuffer, _IOFBF, kIoBufferSize);
    }
    if (outputBuffer != nullptr) {
        std::setvbuf(output, outputBuffer, _IOFBF, kIoBufferSize);
    }

    LineReader inputReader{input};
    LineReader auxReader{aux};
    Stats stats;
    std::string error;

    TimedAuxLine currentAux;
    bool hasAuxLine = false;
    int64_t lastAuxGpst = 0;
    bool hasLastAuxGpst = false;
    bool ok = ReadNextAuxRecord(auxReader, currentAux, hasAuxLine,
                            lastAuxGpst, hasLastAuxGpst, stats, error);

    std::string inputLine;
    std::string inputEol;
    std::string defaultOutputEol = "\r\n";
    int64_t lastTargetGpst = 0;
    bool hasLastTargetGpst = false;
    uint64_t nextProgressBytes = kProgressStepBytes;

    while (ok && inputReader.Read(inputLine, inputEol)) {
        ++stats.inputLines;
        if (!inputEol.empty() && stats.inputLines == 1) {
            defaultOutputEol = inputEol;
        }

        if (inputReader.bytesRead >= nextProgressBytes) {
            std::fprintf(stderr,
                         "Processed input: %.2f GiB, targets=%" PRIu64
                         ", inserted INSPVA=%" PRIu64
                         ", inserted EPH/ION=%" PRIu64 "\n",
                         static_cast<double>(inputReader.bytesRead) /
                             static_cast<double>(1ULL << 30),
                         stats.targetLines, stats.insertedInspvaLines,
                         stats.insertedAuxLines);
            while (nextProgressBytes <= inputReader.bytesRead) {
                nextProgressBytes += kProgressStepBytes;
            }
        }

        const TargetLogType targetType = ClassifyTargetLine(inputLine);
        if (targetType == TargetLogType::kNone) {
            if (!WriteLine(output, inputLine, inputEol)) {
                error = "failed while writing output";
                ok = false;
            }
            continue;
        }

        CountTargetLine(targetType, stats);

        // Only a complete CRC-valid target record is trusted as an insertion
        // anchor. A damaged target line is preserved unchanged and ignored.
        if (!HasValidLongAsciiCrc(inputLine)) {
            if (!WriteLine(output, inputLine, inputEol)) {
                error = "failed while writing CRC-invalid target line";
                ok = false;
            }
            continue;
        }

        ParsedTime targetTime;
        std::string parseError;
        if (!ParseNovatelGpst(inputLine, targetTime, parseError)) {
            if (!WriteLine(output, inputLine, inputEol)) {
                error = "failed while writing malformed target line";
                ok = false;
            }
            continue;
        }
        ++stats.validTargetHeaders;

        if (hasLastTargetGpst && targetTime.gpstNs < lastTargetGpst) {
            error = "input target GPST is not nondecreasing at line " +
                    std::to_string(inputReader.lineNumber) + " (" +
                    TargetLogName(targetType) + ")";
            ok = false;
            break;
        }
        lastTargetGpst = targetTime.gpstNs;
        hasLastTargetGpst = true;

        const std::string &insertEol =
            inputEol.empty() ? defaultOutputEol : inputEol;

        // EPH/ION records are state-bearing navigation data. If an EPH/ION
        // header is older than the current target, keep it and emit it before
        // this target instead of discarding it as stale. INSPVA remains an
        // epoch-matched record, so stale INSPVA is still counted unmatched.
        // This also preserves EPH/ION records that precede the first target in
        // input.log, including GPSCNAVEPH before the first RANGE epoch.
        while (hasAuxLine &&
               currentAux.gpstNs < targetTime.gpstNs - toleranceNs) {
            if (currentAux.kind == AuxRecordKind::kInspva) {
                ++stats.unmatchedInspvaBeforeTarget;
            } else {
                if (!WriteLine(output, currentAux.text, insertEol)) {
                    error = "failed while writing carried-forward EPH/ION line";
                    ok = false;
                    break;
                }
                ++stats.insertedAuxLines;
            }

            ok = ReadNextAuxRecord(auxReader, currentAux, hasAuxLine,
                               lastAuxGpst, hasLastAuxGpst, stats, error);
            if (!ok) {
                break;
            }
        }
        if (!ok) {
            break;
        }

        // Records at the target epoch (or within tolerance) keep their source
        // order and are consumed by the first eligible target, preventing
        // duplicate insertion at later target messages.
        while (hasAuxLine) {
            const int64_t delta = currentAux.gpstNs - targetTime.gpstNs;
            if (delta < -toleranceNs || delta > toleranceNs) {
                break;
            }

            if (!WriteLine(output, currentAux.text, insertEol)) {
                error = "failed while writing inserted auxiliary line";
                ok = false;
                break;
            }

            if (currentAux.kind == AuxRecordKind::kInspva) {
                ++stats.insertedInspvaLines;
            } else {
                ++stats.insertedAuxLines;
            }

            ok = ReadNextAuxRecord(auxReader, currentAux, hasAuxLine,
                               lastAuxGpst, hasLastAuxGpst, stats, error);
            if (!ok) {
                break;
            }
        }
        if (!ok) {
            break;
        }

        if (!WriteLine(output, inputLine, inputEol)) {
            error = "failed while writing target line";
            ok = false;
            break;
        }
    }

    if (ok && std::ferror(input)) {
        error = "failed while reading input log";
        ok = false;
    }

    // Continue to EOF so ordering is validated and final unmatched counts are
    // exact, even when input.log ends before aux.log.
    while (ok && hasAuxLine) {
        if (currentAux.kind == AuxRecordKind::kInspva) {
            ++stats.unmatchedInspvaAfterInput;
        } else {
            ++stats.unmatchedAuxAfterInput;
        }
        ok = ReadNextAuxRecord(auxReader, currentAux, hasAuxLine,
                           lastAuxGpst, hasLastAuxGpst, stats, error);
    }

    if (std::fflush(output) != 0) {
        if (ok) {
            error = "failed to flush output";
        }
        ok = false;
    }

    const int closeOutputResult = std::fclose(output);
    std::fclose(input);
    std::fclose(aux);

    delete[] inputBuffer;
    delete[] auxBuffer;
    delete[] outputBuffer;

    if (closeOutputResult != 0 && ok) {
        error = "failed to close output";
        ok = false;
    }

    if (!ok) {
        std::fprintf(stderr, "ERROR: %s\n", error.c_str());
        std::fprintf(stderr, "Partial output retained at: %s\n",
                     PathForMessage(tempOutputPath).c_str());
        return 4;
    }

    ec.clear();
    if (fs::exists(outputPath, ec)) {
        ec.clear();
        fs::remove(outputPath, ec);
        if (ec) {
            std::fprintf(stderr,
                         "ERROR: cannot replace existing output file: %s\n",
                         ec.message().c_str());
            std::fprintf(stderr, "Completed data remains at: %s\n",
                         PathForMessage(tempOutputPath).c_str());
            return 5;
        }
    }

    ec.clear();
    fs::rename(tempOutputPath, outputPath, ec);
    if (ec) {
        std::fprintf(stderr, "ERROR: cannot rename temporary output: %s\n",
                     ec.message().c_str());
        std::fprintf(stderr, "Completed data remains at: %s\n",
                     PathForMessage(tempOutputPath).c_str());
        return 5;
    }

    std::fprintf(stderr, "\nCompleted successfully.\n");
    std::fprintf(stderr, "  input lines                 : %" PRIu64 "\n",
                 stats.inputLines);
    std::fprintf(stderr, "  eligible target lines       : %" PRIu64 "\n",
                 stats.targetLines);
    std::fprintf(stderr, "  CRC-valid target records    : %" PRIu64 "\n",
                 stats.validTargetHeaders);
    std::fprintf(stderr, "    RANGE                     : %" PRIu64 "\n", stats.rangeLines);
    std::fprintf(stderr, "    BESTPOS                   : %" PRIu64 "\n", stats.bestPosLines);
    std::fprintf(stderr, "    BESTVEL                   : %" PRIu64 "\n", stats.bestVelLines);
    std::fprintf(stderr, "    PSRVEL                    : %" PRIu64 "\n", stats.psrVelLines);
    std::fprintf(stderr, "    PSRPOS                    : %" PRIu64 "\n", stats.psrPosLines);
    std::fprintf(stderr, "  CRC-valid INSPVA records    : %" PRIu64 "\n", stats.inspvaLines);
    std::fprintf(stderr, "  inserted INSPVA lines       : %" PRIu64 "\n", stats.insertedInspvaLines);
    std::fprintf(stderr, "  unmatched INSPVA before     : %" PRIu64 "\n", stats.unmatchedInspvaBeforeTarget);
    std::fprintf(stderr, "  unmatched INSPVA after      : %" PRIu64 "\n", stats.unmatchedInspvaAfterInput);
    std::fprintf(stderr, "  CRC-valid EPH/ION records   : %" PRIu64 "\n", stats.auxLines);
    std::fprintf(stderr, "    NovAtel GLOEPHEMERIS      : %" PRIu64 "\n", stats.novatelGloEphemerisLines);
    std::fprintf(stderr, "    NovAtel QZSSEPHEMERIS     : %" PRIu64 "\n", stats.novatelQzssEphemerisLines);
    std::fprintf(stderr, "    NovAtel GALEPHEMERIS      : %" PRIu64 "\n", stats.novatelGalEphemerisLines);
    std::fprintf(stderr, "    NovAtel GPSEPHEM          : %" PRIu64 "\n", stats.novatelGpsEphemLines);
    std::fprintf(stderr, "    NovAtel GPSL1CEPHEM       : %" PRIu64 "\n", stats.novatelGpsL1cEphemLines);
    std::fprintf(stderr, "    NovAtel BD2EPHEM          : %" PRIu64 "\n", stats.novatelBd2EphemLines);
    std::fprintf(stderr, "    NovAtel IONUTC            : %" PRIu64 "\n", stats.novatelIonUtcLines);
    std::fprintf(stderr, "    NovAtel BD2IONUTC         : %" PRIu64 "\n", stats.novatelBd2IonUtcLines);
    std::fprintf(stderr, "    Unicore GPSION            : %" PRIu64 "\n", stats.unicoreGpsIonLines);
    std::fprintf(stderr, "    Unicore BD3ION            : %" PRIu64 "\n", stats.unicoreBd3IonLines);
    std::fprintf(stderr, "    Unicore BDSION            : %" PRIu64 "\n", stats.unicoreBdsIonLines);
    std::fprintf(stderr, "    Unicore GALION            : %" PRIu64 "\n", stats.unicoreGalIonLines);
    std::fprintf(stderr, "    Unicore GPSEPH            : %" PRIu64 "\n", stats.unicoreGpsEphLines);
    std::fprintf(stderr, "    Unicore GPSCNAVEPH        : %" PRIu64 "\n", stats.unicoreGpsCnavEphLines);
    std::fprintf(stderr, "    Unicore QZSSEPH           : %" PRIu64 "\n", stats.unicoreQzssEphLines);
    std::fprintf(stderr, "    Unicore BD3EPH            : %" PRIu64 "\n", stats.unicoreBd3EphLines);
    std::fprintf(stderr, "    Unicore BDSEPH            : %" PRIu64 "\n", stats.unicoreBdsEphLines);
    std::fprintf(stderr, "    Unicore GLOEPH            : %" PRIu64 "\n", stats.unicoreGloEphLines);
    std::fprintf(stderr, "    Unicore GALEPH            : %" PRIu64 "\n", stats.unicoreGalEphLines);
    std::fprintf(stderr, "    Unicore IRNSSEPH          : %" PRIu64 "\n", stats.unicoreIrnssEphLines);
    std::fprintf(stderr, "  inserted EPH/ION lines      : %" PRIu64 "\n", stats.insertedAuxLines);
    std::fprintf(stderr, "  unmatched EPH/ION before   : %" PRIu64 "\n", stats.unmatchedAuxBeforeTarget);
    std::fprintf(stderr, "  unmatched EPH/ION after    : %" PRIu64 "\n", stats.unmatchedAuxAfterInput);
    std::fprintf(stderr, "  ignored other aux lines     : %" PRIu64 "\n", stats.ignoredAuxLines);
    std::fprintf(stderr, "  output                      : %s\n",
                 PathForMessage(outputPath).c_str());
    return 0;
}

void PrintUsage(const char *program) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s <input.log> <aux.log> <output.log> [tolerance_us]\n\n"
                 "aux.log may contain INSPVA, supported NovAtel/Unicore EPH/ION,\n"
                 "or any mixture of them. The auxiliary file is scanned once.\n"
                 "INSPVA is epoch-matched within tolerance. Earlier EPH/ION records\n"
                 "are preserved and inserted before the next CRC-valid target;\n"
                 "EPH/ION at the target epoch (or within tolerance) is inserted too.\n"
                 "CRC-invalid/incomplete records are silently skipped.\n"
                 "Targets: RANGE/BESTPOS/BESTVEL/PSRVEL/PSRPOS.\n"
                 "Default tolerance is 0 us.\n",
                 program);
}

bool IsHelpOption(const fs::path &arg) {
#ifdef _WIN32
    std::wstring s = arg.wstring();
    for (wchar_t &c : s) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return s == L"-h" || s == L"--help" || s == L"/?";
#else
    std::string s = arg.string();
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s == "-h" || s == "--help" || s == "/?";
#endif
}

int ParseAndRun(int argc, fs::path *args, const char *program) {
    if (argc == 2 && IsHelpOption(args[1])) {
        PrintUsage(program);
        return 0;
    }

    if (argc != 4 && argc != 5) {
        PrintUsage(program);
        return 1;
    }

    int64_t toleranceNs = 0;
    if (argc == 5) {
        bool toleranceOk = false;
        toleranceNs = ParseToleranceUs(args[4], toleranceOk);
        if (!toleranceOk) {
            std::fprintf(stderr,
                         "ERROR: tolerance_us must be an integer in [0, 1000000].\n");
            return 1;
        }
    }

    return Run(args[1], args[2], args[3], toleranceNs);
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t *argv[]) {
    fs::path *args = new (std::nothrow) fs::path[static_cast<size_t>(argc)];
    if (args == nullptr) {
        std::fprintf(stderr, "ERROR: out of memory.\n");
        return 1;
    }
    for (int i = 0; i < argc; ++i) args[i] = fs::path(argv[i]);
    const int result = ParseAndRun(argc, args, "merge_aux_into_input.exe");
    delete[] args;
    return result;
}
#else
int main(int argc, char *argv[]) {
    fs::path *args = new (std::nothrow) fs::path[static_cast<size_t>(argc)];
    if (args == nullptr) {
        std::fprintf(stderr, "ERROR: out of memory.\n");
        return 1;
    }
    for (int i = 0; i < argc; ++i) args[i] = fs::path(argv[i]);
    const int result = ParseAndRun(argc, args, argv[0]);
    delete[] args;
    return result;
}
#endif
