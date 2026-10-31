/*
   Copyright (c) 2013, The Linux Foundation. All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
   ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
   BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
   OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
   IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "vendor_init.h"
#include "property_service.h"
#include "log.h"

namespace {

struct Evidence {
    std::set<std::string> models;
    std::set<std::string> versions;
};

bool token_char(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '+';
}

void inspect(const std::string& text, bool firmware, Evidence* evidence) {
    for (size_t i = 0; i + 4 <= text.size(); ++i) {
        if (toupper(static_cast<unsigned char>(text[i])) != 'E' ||
            (i && isalnum(static_cast<unsigned char>(text[i - 1])))) continue;
        std::string base = text.substr(i, 4);
        base[0] = 'E';
        // These are the retail variants supported by this Vee3 device tree.
        if (base != "E425" && base != "E430" && base != "E431" && base != "E435") continue;
        size_t end = i + 4;
        std::string model = "LG-" + base;
        if (end < text.size() && isalpha(static_cast<unsigned char>(text[end]))) {
            if (end + 1 < text.size() && isalnum(static_cast<unsigned char>(text[end + 1]))) continue;
            model += static_cast<char>(tolower(static_cast<unsigned char>(text[end])));
            ++end;
        }
        if (end < text.size() && isalnum(static_cast<unsigned char>(text[end]))) continue;
        if (firmware) {
            if (end >= text.size() || (text[end] != '-' && text[end] != '_')) continue;
            size_t stop = end;
            while (stop < text.size() && token_char(text[stop]) && stop - i < PROP_VALUE_MAX - 1U) ++stop;
            std::string version = text.substr(i, stop - i);
            size_t marker = version.find("-V");
            if (marker == std::string::npos || marker + 3 >= version.size() ||
                !isdigit(static_cast<unsigned char>(version[marker + 2])) ||
                !isdigit(static_cast<unsigned char>(version[marker + 3]))) continue;
            if (evidence->versions.size() < 16) evidence->versions.insert(version);
        }
        evidence->models.insert(model);
    }
}

std::string resolve(const Evidence& evidence) {
    std::string base, qualified;
    for (std::set<std::string>::const_iterator i = evidence.models.begin(); i != evidence.models.end(); ++i) {
        if (base.empty()) base = i->substr(0, 7);
        if (i->substr(0, 7) != base) return "";
        if (i->size() > 7) {
            if (!qualified.empty() && qualified != *i) return "";
            qualified = *i;
        }
    }
    return qualified.empty() ? base : qualified;
}

std::string read_text(const std::string& path, size_t limit) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    std::string text;
    char buffer[4096];
    while (text.size() < limit) {
        size_t wanted = limit - text.size();
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        ssize_t count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        text.append(buffer, static_cast<size_t>(count));
    }
    close(fd);
    return text;
}

void inspect_factory(const std::string& path, Evidence* evidence) {
    std::istringstream input(read_text(path, 65536));
    std::string line;
    while (std::getline(input, line)) {
        size_t split = line.find('=');
        if (split == std::string::npos) continue;
        std::string key = line.substr(0, split);
        if (key == "ro.product.model" || key == "ro.lge.model" || key == "ro.lge.model_name")
            inspect(line.substr(split + 1), false, evidence);
        if (key == "ro.lge.swversion") inspect(line.substr(split + 1), true, evidence);
    }
}

std::vector<std::string> directory_entries(const std::string& path) {
    std::vector<std::string> entries;
    DIR* directory = opendir(path.c_str());
    if (!directory) return entries;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] != '.') entries.push_back(entry->d_name);
    }
    closedir(directory);
    return entries;
}

void append_named(const std::string& directory, std::vector<std::string>* paths) {
    const char* names[] = {"modem", "radio", "amss"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        std::string path = directory + "/" + names[i];
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) paths->push_back(path);
    }
}

bool modem_name(const std::string& name) {
    return name == "modem" || name == "radio" || name == "amss";
}

std::vector<std::string> modem_paths(bool* legacy) {
    std::vector<std::string> paths;
    append_named("/dev/block/by-name", &paths);
    append_named("/dev/block/bootdevice/by-name", &paths);
    std::vector<std::string> controllers = directory_entries("/dev/block/platform");
    for (size_t i = 0; i < controllers.size(); ++i)
        append_named("/dev/block/platform/" + controllers[i] + "/by-name", &paths);
    std::vector<std::string> blocks = directory_entries("/sys/class/block");
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].compare(0, 6, "mmcblk") != 0) continue;
        std::istringstream input(read_text("/sys/class/block/" + blocks[i] + "/uevent", 4096));
        std::string line, name, device;
        while (std::getline(input, line)) {
            if (line.compare(0, 9, "PARTNAME=") == 0) name = line.substr(9);
            if (line.compare(0, 8, "DEVNAME=") == 0) device = line.substr(8);
        }
        if (modem_name(name) && device.compare(0, 6, "mmcblk") == 0 &&
            device.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos)
            paths.push_back("/dev/block/" + device);
    }
    // Old Vee3 bootloaders use an unlabeled MBR. Keep the existing DT map as
    // a fallback only; model identity is still read from the phone's firmware.
    *legacy = paths.empty();
    if (*legacy) paths.push_back("/dev/block/mmcblk0p12");
    return paths;
}

void inspect_modem(const std::string& path, std::set<dev_t>* seen, Evidence* evidence) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode) || !seen->insert(st.st_rdev).second) {
        close(fd);
        return;
    }
    const size_t limit = 128U * 1024U * 1024U;
    size_t total = 0;
    char buffer[16384];
    std::string printable;
    Evidence local;
    bool complete = false;
    while (total < limit) {
        size_t wanted = limit - total;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        ssize_t count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) { complete = true; break; }
        if (count < 0) break;
        total += static_cast<size_t>(count);
        for (ssize_t i = 0; i < count; ++i) {
            unsigned char c = static_cast<unsigned char>(buffer[i]);
            if (c >= 32 && c <= 126) {
                printable += static_cast<char>(c);
                if (printable.size() == 512) {
                    inspect(printable, true, &local);
                    printable.erase(0, 256);
                }
            } else {
                if (printable.size() >= 7) inspect(printable, true, &local);
                printable.clear();
            }
        }
    }
    if (total == limit) {
        char extra;
        complete = read(fd, &extra, 1) == 0;
    }
    close(fd);
    if (!complete) {
        NOTICE("Vee3: modem identity scan incomplete; keeping this source untrusted\n");
        return;
    }
    inspect(printable, true, &local);
    evidence->models.insert(local.models.begin(), local.models.end());
    evidence->versions.insert(local.versions.begin(), local.versions.end());
}

void override_property(const char* name, const std::string& value) {
    if (value.size() >= PROP_VALUE_MAX) return;
    prop_info* property = const_cast<prop_info*>(__system_property_find(name));
    int result = property ? __system_property_update(property, value.c_str(), value.size()) :
        __system_property_add(name, strlen(name), value.c_str(), value.size());
    if (result != 0) ERROR("Vee3: unable to publish property %s\n", name);
}

}  // namespace

void vendor_load_properties() {
    Evidence evidence;
    std::string source = "bootloader";
    const char* boot_keys[] = {"ro.boot.model", "ro.boot.product.model", "ro.boot.lge.model",
        "ro.boot.lge_model_name", "ro.boot.hardware.sku"};
    for (size_t i = 0; i < sizeof(boot_keys) / sizeof(boot_keys[0]); ++i)
        inspect(property_get(boot_keys[i]), false, &evidence);
    std::istringstream command_line(read_text("/proc/cmdline", 8192));
    std::string token;
    while (command_line >> token) {
        size_t equal = token.find('=');
        if (equal == std::string::npos) continue;
        std::string key = token.substr(0, equal);
        if (key == "lge.model" || key == "lge.model_name" || key == "androidboot.model" ||
            key == "androidboot.lge_model_name") inspect(token.substr(equal + 1), false, &evidence);
    }
    if (evidence.models.empty()) {
        source = "factory";
        inspect_factory("/factory/factory.prop", &evidence);
        inspect_factory("/mpt/factory.prop", &evidence);
    }
    if (evidence.models.empty()) {
        bool legacy = false;
        std::vector<std::string> paths = modem_paths(&legacy);
        std::set<dev_t> seen;
        source = legacy ? "modem_legacy_map" : "modem_named";
        for (size_t i = 0; i < paths.size(); ++i) inspect_modem(paths[i], &seen, &evidence);
    }
    std::string model = resolve(evidence);
    std::string status = !model.empty() ? "detected" :
        evidence.models.empty() ? "not_found" : "ambiguous";
    // Product properties already exist here on CM 14.1. Update them in init;
    // a later shell setprop cannot replace an existing read-only property.
    override_property("ro.product.device", "vee3");
    override_property("ro.product.model", model.empty() ? "LG Optimus L3 II" : model);
    override_property("ro.lge.variant.source", source);
    override_property("ro.lge.variant.status", status);
    if (!model.empty()) {
        // Preserve the original DT's dual-SIM rule. Do not erase existing
        // persistent radio settings on single-SIM or unknown variants.
        if (model.compare(0, 7, "LG-E435") == 0) {
            property_set("persist.radio.multisim.config", "dsds");
            property_set("persist.multisim.config", "dsds");
            override_property("ro.multi.rild", "true");
        }
        if (evidence.versions.size() == 1 && property_get("gsm.version.baseband").empty())
            property_set("gsm.version.baseband", evidence.versions.begin()->c_str());
    }
    NOTICE("Vee3: model=%s source=%s status=%s\n",
        model.empty() ? "unknown" : model.c_str(), source.c_str(), status.c_str());
}
