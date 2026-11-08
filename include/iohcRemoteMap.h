#ifndef IOHC_REMOTE_MAP_H
#define IOHC_REMOTE_MAP_H

#include <iohcPacket.h>
#include <vector>
#include <functional>
#include <string>

#define REMOTE_MAP_FILE "/RemoteMap.json"

namespace IOHC {
    class iohcRemoteMap {
    public:
        struct entry {
            address node;
            std::string name;
            std::vector<std::string> devices;
        };

        static iohcRemoteMap* getInstance();
        ~iohcRemoteMap() = default;

        const entry* find(const address node) const;
        bool load();
        bool add(const address node, const std::string &name);
        bool linkDevice(const address node, const std::string &device);
        bool unlinkDevice(const address node, const std::string &device);
        bool renameDevice(const address node, const std::string &name);
        bool remove(const address node);
        const std::vector<entry>& getEntries() const;
        // Calls fn once per device description linked to node. The link
        // kAllDevices stands for every 1W device, including ones added later.
        void forEachLinkedDevice(const address node, const std::function<void(const std::string &)> &fn) const;
        static constexpr const char *kAllDevices = "ALL";

    private:
        iohcRemoteMap();
        bool save();
        static iohcRemoteMap* _instance;
        std::vector<entry> _entries;
    };
}

#endif // IOHC_REMOTE_MAP_H
