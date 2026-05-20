// SPDX-License-Identifier: GPL-2.0
/*
 * myfs_test.cpp — программа для проверки работы myfs.
 *
 * Команды:
 *   myfs_test test     <mountpoint>           — обойти все файлы, записать
 *                                                и прочитать случайное число
 *   myfs_test zero     <mountpoint>           — IOCTL: обнулить все файлы
 *   myfs_test erase    <mountpoint>           — IOCTL: стереть ФС
 *   myfs_test hashes   <mountpoint>           — IOCTL: получить хэши всех файлов
 *   myfs_test mapping  <mountpoint> <name>    — IOCTL: получить маппинг файла
 *   myfs_test ls       <mountpoint>           — просто показать файлы
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "myfs.h"

namespace fs = std::filesystem;

/* ---------- утилиты ---------- */

static int open_dir(const std::string& mp) {
    int fd = ::open(mp.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) std::perror(("open " + mp).c_str());
    return fd;
}

static std::vector<fs::path> list_files(const std::string& mp) {
    std::vector<fs::path> result;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(mp, ec)) {
        if (ec) {
            std::cerr << "directory_iterator: " << ec.message() << "\n";
            break;
        }
        if (e.is_regular_file()) result.push_back(e.path());
    }
    std::sort(result.begin(), result.end(),
              [](const fs::path& a, const fs::path& b) {
        /* сортируем по числовому суффиксу file_NNN */
        auto extract = [](const std::string& s) -> long {
            auto pos = s.rfind('_');
            return pos == std::string::npos ? 0 : std::atol(s.c_str() + pos + 1);
        };
        return extract(a.filename().string()) < extract(b.filename().string());
    });
    return result;
}

/* ---------- команды ---------- */

static int cmd_ls(const std::string& mp) {
    auto files = list_files(mp);
    std::cout << "Files in " << mp << " (" << files.size() << "):\n";
    for (const auto& p : files) std::cout << "  " << p.filename().string() << "\n";
    return 0;
}

static int cmd_test(const std::string& mp) {
    auto files = list_files(mp);
    if (files.empty()) {
        std::cerr << "No files in " << mp << "\n";
        return 1;
    }

    std::mt19937_64 rng(std::random_device{}());
    size_t total = files.size();
    size_t passed = 0;
    size_t failed = 0;

    std::cout << "Testing " << total << " files in " << mp << " ...\n";

    for (const auto& p : files) {
        std::string path = p.string();
        uint64_t value = rng();

        int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0) {
            std::cerr << "  [WRITE-OPEN] " << path << ": "
                      << std::strerror(errno) << "\n";
            ++failed;
            continue;
        }
        ssize_t w = ::pwrite(fd, &value, sizeof(value), 0);
        ::close(fd);
        if (w != (ssize_t)sizeof(value)) {
            std::cerr << "  [WRITE]      " << path << ": "
                      << std::strerror(errno) << " (wrote " << w << ")\n";
            ++failed;
            continue;
        }

        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "  [READ-OPEN]  " << path << ": "
                      << std::strerror(errno) << "\n";
            ++failed;
            continue;
        }
        uint64_t readback = 0;
        ssize_t r = ::pread(fd, &readback, sizeof(readback), 0);
        ::close(fd);
        if (r != (ssize_t)sizeof(readback)) {
            std::cerr << "  [READ]       " << path << ": "
                      << std::strerror(errno) << " (read " << r << ")\n";
            ++failed;
            continue;
        }

        if (readback == value) {
            ++passed;
            if (total <= 32 || passed % 100 == 0) {
                std::printf("  OK   %-24s 0x%016lx\n",
                            p.filename().string().c_str(),
                            (unsigned long)value);
            }
        } else {
            std::printf("  FAIL %s: wrote 0x%016lx, got 0x%016lx\n",
                        p.filename().string().c_str(),
                        (unsigned long)value, (unsigned long)readback);
            ++failed;
        }
    }

    std::cout << "Result: " << passed << " OK, " << failed << " FAIL ("
              << total << " total)\n";
    return failed == 0 ? 0 : 2;
}

static int cmd_zero(const std::string& mp) {
    int fd = open_dir(mp);
    if (fd < 0) return 1;
    int rc = ::ioctl(fd, MYFS_IOC_ZERO_ALL);
    int saved = errno;
    ::close(fd);
    if (rc != 0) {
        std::cerr << "ioctl ZERO_ALL: " << std::strerror(saved) << "\n";
        return 1;
    }
    std::cout << "All files zeroed.\n";
    return 0;
}

static int cmd_erase(const std::string& mp) {
    int fd = open_dir(mp);
    if (fd < 0) return 1;
    int rc = ::ioctl(fd, MYFS_IOC_ERASE_FS);
    int saved = errno;
    ::close(fd);
    if (rc != 0) {
        std::cerr << "ioctl ERASE_FS: " << std::strerror(saved) << "\n";
        return 1;
    }
    std::cout << "FS erased. Unmount the FS to confirm.\n";
    return 0;
}

static int cmd_hashes(const std::string& mp) {
    int fd = open_dir(mp);
    if (fd < 0) return 1;

    auto* h = (struct myfs_hashes*)std::calloc(1, sizeof(struct myfs_hashes));
    if (!h) { ::close(fd); std::perror("calloc"); return 1; }

    h->count    = 0;
    h->capacity = MYFS_MAX_HASHES;

    int rc = ::ioctl(fd, MYFS_IOC_GET_HASHES, h);
    int saved = errno;
    ::close(fd);

    if (rc != 0) {
        std::cerr << "ioctl GET_HASHES: " << std::strerror(saved) << "\n";
        std::free(h);
        return 1;
    }

    std::cout << "File hashes (" << h->count << "):\n";
    for (uint32_t i = 0; i < h->count; ++i) {
        std::printf("  %-24s CRC32=0x%08x\n",
                    h->entries[i].name,
                    (unsigned)h->entries[i].hash);
    }
    std::free(h);
    return 0;
}

static int cmd_mapping(const std::string& mp, const std::string& name) {
    int fd = open_dir(mp);
    if (fd < 0) return 1;

    struct myfs_mapping m;
    std::memset(&m, 0, sizeof(m));
    std::strncpy(m.name, name.c_str(), sizeof(m.name) - 1);

    int rc = ::ioctl(fd, MYFS_IOC_GET_MAPPING, &m);
    int saved = errno;
    ::close(fd);

    if (rc != 0) {
        std::cerr << "ioctl GET_MAPPING: " << std::strerror(saved) << "\n";
        return 1;
    }

    std::cout << "Mapping for \"" << name << "\":\n";
    std::cout << "  file_index  = " << m.file_index  << "\n";
    std::cout << "  start_block = " << m.start_block << "\n";
    std::cout << "  num_blocks  = " << m.num_blocks  << "\n";
    std::cout << "  block range = [" << m.start_block
              << ", " << (m.start_block + m.num_blocks) << ")\n";
    std::cout << "  byte range  = [" << (uint64_t)m.start_block * MYFS_BLOCK_SIZE
              << ", " << (uint64_t)(m.start_block + m.num_blocks) * MYFS_BLOCK_SIZE
              << ")\n";
    return 0;
}

static void usage(const char* prog) {
    std::cout
      << "Usage:\n"
      << "  " << prog << " test    <mountpoint>\n"
      << "  " << prog << " ls      <mountpoint>\n"
      << "  " << prog << " zero    <mountpoint>\n"
      << "  " << prog << " erase   <mountpoint>\n"
      << "  " << prog << " hashes  <mountpoint>\n"
      << "  " << prog << " mapping <mountpoint> <name>\n";
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    std::string cmd = argv[1];
    std::string mp  = argv[2];

    if      (cmd == "test")    return cmd_test(mp);
    else if (cmd == "ls")      return cmd_ls(mp);
    else if (cmd == "zero")    return cmd_zero(mp);
    else if (cmd == "erase")   return cmd_erase(mp);
    else if (cmd == "hashes")  return cmd_hashes(mp);
    else if (cmd == "mapping") {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_mapping(mp, argv[3]);
    }
    usage(argv[0]);
    return 1;
}
