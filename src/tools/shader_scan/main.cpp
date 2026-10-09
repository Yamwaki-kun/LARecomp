#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "mc_engine/modloader/rpf3.h"
#include "mc_engine/modloader/xcompress.h"
#include "native_gfx/shader_identity.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "mc_engine/modloader/stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;
using mc::modloader::Rpf3Entry;
using mc::modloader::Rpf3Reader;

namespace {

constexpr uint32_t kRsc5Magic = 0x05435352u;
constexpr uint32_t kXCompressMagic = 0x0FF512EFu;
constexpr uint64_t kMaxLogicalEntry = 1ull << 30;
constexpr size_t kContainerHeaderSize = 36;
constexpr size_t kConstantTableContainerSize = 32;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

std::string Hex64(uint64_t value) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string Hex32(uint32_t value) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
  return out.str();
}

std::string Csv(std::string value) {
  size_t pos = 0;
  while ((pos = value.find('"', pos)) != std::string::npos) {
    value.insert(pos, 1, '"');
    pos += 2;
  }
  return '"' + value + '"';
}

bool InflateRaw(const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out) {
  out.clear();
  constexpr size_t kIntMax = size_t(std::numeric_limits<int>::max());
  if (!data || size == 0 || expected == 0 || size > kIntMax || expected > kIntMax) return false;
  out.resize(expected);
  const int result = stbi_zlib_decode_noheader_buffer(
      reinterpret_cast<char*>(out.data()), int(expected),
      reinterpret_cast<const char*>(data), int(size));
  if (result != int(expected)) {
    out.clear();
    return false;
  }
  return true;
}

bool DecodeEntry(const Rpf3Entry& entry, const std::vector<uint8_t>& stored,
                 std::vector<uint8_t>& logical, std::string& codec) {
  logical.clear();
  if (!entry.is_resource()) {
    if (!(entry.flag & 0x40000000u)) {
      logical = stored;
      codec = "raw";
      return true;
    }
    codec = "deflate";
    return InflateRaw(stored.data(), stored.size(), entry.size, logical);
  }

  const bool compressed = (entry.flag & 0x40000000u) != 0;
  const size_t header_size = compressed ? 20u : 12u;
  codec = compressed ? "rsc5-lzx" : "rsc5-raw";
  if (stored.size() < header_size || LoadBE32(stored.data()) != kRsc5Magic) return false;
  const uint32_t flag = LoadBE32(stored.data() + 8);
  const uint64_t virtual_size = uint64_t(flag & 0x7FFu) << (((flag >> 11) & 0xFu) + 8);
  const uint64_t physical_size = uint64_t((flag >> 15) & 0x7FFu) << (((flag >> 26) & 0xFu) + 8);
  const uint64_t logical_size = virtual_size + physical_size;
  if (logical_size == 0 || logical_size > kMaxLogicalEntry || logical_size > SIZE_MAX) return false;

  if (!compressed) {
    if (stored.size() - header_size < logical_size) return false;
    logical.assign(stored.begin() + ptrdiff_t(header_size),
                   stored.begin() + ptrdiff_t(header_size + size_t(logical_size)));
    return true;
  }
  if (LoadBE32(stored.data() + 12) != kXCompressMagic) return false;
  const uint32_t compressed_size = LoadBE32(stored.data() + 16);
  if (compressed_size == 0 || uint64_t(header_size) + compressed_size > stored.size()) return false;
  return mc::modloader::LzxDecompress(stored.data() + header_size, compressed_size, logical,
                                      size_t(logical_size));
}

struct ContainerInfo {
  size_t size = 0;
  bool pixel = false;
  size_t ucode_offset = 0;
  size_t ucode_size = 0;
  uint64_t identity = 0;
  uint64_t container_hash = 0;
};

bool TryContainer(const uint8_t* data, size_t available, ContainerInfo& out) {
  if (available < kContainerHeaderSize) return false;
  const uint32_t flags = LoadBE32(data);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return false;
  const uint32_t virtual_size = LoadBE32(data + 4);
  const uint32_t physical_size = LoadBE32(data + 8);
  const uint64_t total = uint64_t(virtual_size) + physical_size;
  const uint32_t constant_table = LoadBE32(data + 16);
  const uint32_t shader_offset = LoadBE32(data + 24);
  if (total < kContainerHeaderSize || total > available || total > SIZE_MAX ||
      LoadBE32(data + 28) != 0 || LoadBE32(data + 32) != 0 ||
      constant_table == 0 || uint64_t(constant_table) + kConstantTableContainerSize > virtual_size ||
      shader_offset == 0 || uint64_t(shader_offset) + 24 > virtual_size) {
    return false;
  }
  const uint32_t constant_count = LoadBE32(data + constant_table + 16);
  const uint32_t constant_info = LoadBE32(data + constant_table + 20);
  if (constant_count > 1024 ||
      uint64_t(constant_table) + 4 + constant_info + uint64_t(constant_count) * 20 > virtual_size) {
    return false;
  }
  const uint32_t physical_offset = LoadBE32(data + shader_offset);
  const uint32_t ucode_size = LoadBE32(data + shader_offset + 4);
  const uint64_t ucode_offset = uint64_t(virtual_size) + physical_offset;
  if (ucode_size == 0 || ucode_size > mcla::native_gfx::kMaxUcodeBytes ||
      (ucode_size & 3u) != 0 || ucode_offset > total || ucode_size > total - ucode_offset) {
    return false;
  }
  const bool pixel = (flags & 1u) == 0;
  if (!pixel) {
    if (uint64_t(shader_offset) + 36 > virtual_size) return false;
    const uint32_t first_element = LoadBE32(data + shader_offset + 24);
    const uint32_t element_count = LoadBE32(data + shader_offset + 28);
    if (element_count > 64 ||
        uint64_t(shader_offset) + 36 + uint64_t(first_element + element_count) * 4 > virtual_size) {
      return false;
    }
  }
  out.size = size_t(total);
  out.pixel = pixel;
  out.ucode_offset = size_t(ucode_offset);
  out.ucode_size = ucode_size;
  out.identity = mcla::native_gfx::ShaderIdentity(data + out.ucode_offset, out.ucode_size);
  out.container_hash = mcla::native_gfx::Fnv1a64(data, out.size);
  return out.identity != 0;
}

struct Stats {
  uint64_t archives = 0;
  uint64_t entries = 0;
  uint64_t decoded = 0;
  uint64_t decode_failures = 0;
  uint64_t occurrences = 0;
  std::unordered_set<uint64_t> containers;
  std::set<std::pair<uint64_t, bool>> identities;
};

bool IsArchive(const fs::path& path) {
  std::string name = path.filename().string();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return name.starts_with("xarchive_") && name.ends_with(".rpf");
}

bool WriteBlob(const fs::path& path, const uint8_t* data, size_t size) {
  if (fs::exists(path)) return true;
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(data), std::streamsize(size));
  return stream.good();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: larecomp_shader_scan <game-data-directory> <output-directory>\n";
    return 1;
  }
  const fs::path input = fs::absolute(argv[1]);
  const fs::path output = fs::absolute(argv[2]);
  if (!fs::is_directory(input)) {
    std::cerr << "Input is not a directory: " << input << '\n';
    return 1;
  }
  if (input.lexically_normal() == output.lexically_normal()) {
    std::cerr << "Output must not be the game data directory\n";
    return 1;
  }
  std::error_code ec;
  fs::create_directories(output / "containers", ec);
  if (ec) {
    std::cerr << "Cannot create output: " << ec.message() << '\n';
    return 1;
  }

  std::vector<fs::path> archives;
  for (const auto& item : fs::directory_iterator(input)) {
    if (item.is_regular_file() && IsArchive(item.path())) archives.push_back(item.path());
  }
  std::sort(archives.begin(), archives.end());
  if (archives.empty()) {
    std::cerr << "No xarchive_*.rpf files found in " << input << '\n';
    return 1;
  }

  std::ofstream manifest(output / "manifest.csv", std::ios::binary);
  manifest << "archive,entry_index,entry_hash,codec,logical_bytes,container_offset,container_bytes,stage,identity,container_hash,file\n";
  Stats stats;

  for (const fs::path& archive : archives) {
    Rpf3Reader reader;
    if (!reader.Open(archive)) {
      std::cerr << "Cannot open " << archive.filename() << '\n';
      continue;
    }
    ++stats.archives;
    std::cout << "Scanning " << archive.filename().string() << " (" << reader.entry_count() << " entries)\n";
    for (size_t index = 0; index < reader.entry_count(); ++index) {
      const Rpf3Entry* entry = reader.entry_at(index);
      if (!entry || entry->is_directory()) continue;
      ++stats.entries;
      std::vector<uint8_t> stored;
      std::vector<uint8_t> logical;
      if (!reader.ReadStoredFile(index, stored)) {
        ++stats.decode_failures;
        continue;
      }
      std::string codec;
      if (!DecodeEntry(*entry, stored, logical, codec)) {
        ++stats.decode_failures;
        continue;
      }
      ++stats.decoded;
      for (size_t offset = 0; offset + kContainerHeaderSize <= logical.size();) {
        ContainerInfo info;
        if (!TryContainer(logical.data() + offset, logical.size() - offset, info)) {
          ++offset;  // Containers in the XEX are demonstrably not always dword-aligned.
          continue;
        }
        ++stats.occurrences;
        stats.containers.insert(info.container_hash);
        stats.identities.emplace(info.identity, info.pixel);
        const std::string identity = Hex64(info.identity);
        const std::string full_hash = Hex64(info.container_hash);
        const std::string stage = info.pixel ? "ps" : "vs";
        const std::string filename = identity + "_" + stage + "_" + full_hash + ".bin";
        if (!WriteBlob(output / "containers" / filename, logical.data() + offset, info.size)) {
          std::cerr << "Cannot write " << filename << '\n';
          return 2;
        }
        manifest << Csv(archive.filename().string()) << ',' << index << ',' << Hex32(entry->hash)
                 << ',' << codec << ',' << logical.size() << ',' << offset << ',' << info.size
                 << ',' << stage << ',' << identity << ',' << full_hash << ',' << Csv(filename) << '\n';
        offset += info.size;
      }
    }
  }

  std::ofstream summary(output / "summary.csv", std::ios::binary);
  summary << "key,value\narchives," << stats.archives << "\nentries," << stats.entries
          << "\ndecoded_entries," << stats.decoded << "\ndecode_failures," << stats.decode_failures
          << "\ncontainer_occurrences," << stats.occurrences << "\nunique_containers,"
          << stats.containers.size() << "\nunique_shader_identities," << stats.identities.size() << '\n';
  std::cout << "Found " << stats.occurrences << " container occurrences, " << stats.containers.size()
            << " unique containers, " << stats.identities.size() << " shader identities\n";
  return stats.occurrences ? 0 : 2;
}
