#ifndef _DISCARDERS_H
#define _DISCARDERS_H

#define REVISION_ARRAY_SIZE 4096

struct bpf_map_def SEC("maps/discarder_revisions") discarder_revisions = {
    .type = BPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(u32),
    .value_size = sizeof(u32),
    .max_entries = REVISION_ARRAY_SIZE,
    .pinning = 0,
    .namespace = "",
};

int __attribute__((always_inline)) get_discarder_revision(u32 mount_id) {
    u32 i = mount_id % REVISION_ARRAY_SIZE;
    u32 *revision = bpf_map_lookup_elem(&discarder_revisions, &i);

    return revision ? *revision : 0;
}

int __attribute__((always_inline)) bump_discarder_revision(u32 mount_id) {
    u32 i = mount_id % REVISION_ARRAY_SIZE;
    u32 *revision = bpf_map_lookup_elem(&discarder_revisions, &i);
    if (!revision) {
        return 0;
    }

    // bump only already > 0 meaning that the user space decided that for this mount_id
    // all the discarders will be invalidated
    if (*revision > 0) {
        if (*revision + 1 == 0) {
            __sync_fetch_and_add(revision, 2); // handle overflow
        } else {
            __sync_fetch_and_add(revision, 1);
        }
    }

    return *revision;
}

struct discarder_state_t {
    u64 expire_at;
    u32 is_invalid;
};

struct discarder_parameters_t {
    u64 event_mask;
    u64 timestamps[EVENT_MAX_ROUNDED_UP];
    struct discarder_state_t state;
    u32 padding;
};

void __attribute__((always_inline)) discard(struct bpf_map_def *discarder_map, void *key, u64 event_type, u64 timeout) {
    u64 timestamp = timeout ? bpf_ktime_get_ns() + timeout : 0;

    struct discarder_parameters_t *params = bpf_map_lookup_elem(discarder_map, key);
    if (params) {
        params->event_mask |= event_type;
        params->timestamps[(event_type-1)&(EVENT_MAX_ROUNDED_UP-1)] = timestamp;
    } else {
        struct discarder_parameters_t new_params = {
            .event_mask = event_type,
        };

        void *dst = new_params.timestamps + ((event_type-1)&(EVENT_MAX_ROUNDED_UP-1));
        bpf_probe_read(dst, sizeof(u64), &timestamp);

        bpf_map_update_elem(discarder_map, key, &new_params, BPF_NOEXIST);
    }
}

int __attribute__((always_inline)) is_discarded(struct bpf_map_def *discarder_map, void *key, u64 event_type) {
    struct discarder_parameters_t *params = bpf_map_lookup_elem(discarder_map, key);
    if (!params) {
        return 0;
    }

    u64 tm = bpf_ktime_get_ns();

    // this discarder has been marked as invalid by event such as unlink, rename, etc.
    // keep them for a while in the map to avoid userspace to reinsert it
    if (params->state.is_invalid) {
        if (params->state.expire_at < tm) {
            bpf_map_delete_elem(discarder_map, key);
        }
        return 0;
    }

    u64 pid_tm = params->timestamps[(event_type-1)&(EVENT_MAX_ROUNDED_UP-1)];
    if (event_type > 0 && pid_tm != 0 && pid_tm <= tm) {
        return 0;
    }

    if (mask_has_event(params->event_mask, event_type)) {
        return 1;
    }

    return 0;
}

// do not remove it direclty, first mark it as invalid for a period of time, after that it will be removed
void __attribute__((always_inline)) remove_discarder(struct bpf_map_def *discarder_map, void *key) {
    struct discarder_parameters_t *params = bpf_map_lookup_elem(discarder_map, key);
    if (params) {
        params->state.is_invalid = 1;
        params->state.expire_at = bpf_ktime_get_ns() + DISCARDER_RETENTION;
    }
}

struct inode_discarder_t {
    struct path_key_t path_key;
    u32 revision;
    u32 padding;
};

struct inode_filter_t {
    struct discarder_parameters_t params;
    u64 parent_mask;
    u64 leaf_mask;
    u64 timestamp;
    u32 is_invalid;
    u32 padding;
};

struct bpf_map_def SEC("maps/inode_discarders") inode_discarders = {
    .type = BPF_MAP_TYPE_LRU_HASH,
    .key_size = sizeof(struct inode_discarder_t),
    .value_size = sizeof(struct discarder_parameters_t),
    .max_entries = 512,
    .pinning = 0,
    .namespace = "",
};

int __attribute__((always_inline)) discard_inode(u64 event_type, u32 mount_id, u64 inode, u64 timeout) {
    struct inode_discarder_t key = {
        .path_key = {
            .ino = inode,
            .mount_id = mount_id,
        },
        .revision = get_discarder_revision(mount_id),
    };

    discard(&inode_discarders, &key, event_type, timeout);

    return 0;
}

int __attribute__((always_inline)) is_discarded_by_inode(u64 event_type, u32 mount_id, u64 inode, int depth) {
    struct inode_discarder_t key = {
        .path_key = {
            .ino = inode,
            .mount_id = mount_id,
        },
        .revision = get_discarder_revision(mount_id),
    };

    return is_discarded(&inode_discarders, &key, event_type);
}

void __attribute__((always_inline)) remove_inode_discarder(u32 mount_id, u64 inode) {
    struct inode_discarder_t key = {
        .path_key = {
            .ino = inode,
            .mount_id = mount_id,
        },
        .revision = get_discarder_revision(mount_id),
    };

    remove_discarder(&inode_discarders, &key);
}

static __always_inline u32 get_system_probe_pid() {
    u64 val = 0;
    LOAD_CONSTANT("system_probe_pid", val);
    return val;
}

struct pid_discarder_t {
    u32 tgid;
};

struct bpf_map_def SEC("maps/pid_discarders") pid_discarders = { \
    .type = BPF_MAP_TYPE_LRU_HASH,
    .key_size = sizeof(u32),
    .value_size = sizeof(struct discarder_parameters_t),
    .max_entries = 512,
    .pinning = 0,
    .namespace = "",
};

int __attribute__((always_inline)) discard_pid(u64 event_type, u32 tgid, u64 timeout) {
    struct pid_discarder_t key = {
        .tgid = tgid,
    };

    discard(&pid_discarders, &key, event_type, timeout);

    return 0;
}

int __attribute__((always_inline)) is_discarded_by_pid(u64 event_type, u32 tgid) {
    u32 system_probe_pid = get_system_probe_pid();
    if (system_probe_pid && system_probe_pid == tgid) {
        return 1;
    }

    struct pid_discarder_t key = {
        .tgid = tgid,
    };

    return is_discarded(&pid_discarders, &key, event_type);
}

int __attribute__((always_inline)) is_discarded_by_process(const char mode, u64 event_type) {
    if (mode != NO_FILTER) {
        u64 pid_tgid = bpf_get_current_pid_tgid();
        u32 tgid = pid_tgid >> 32;

        // try with pid first
        if (is_discarded_by_pid(event_type, tgid))
            return 1;

        struct proc_cache_t *entry = get_proc_cache(tgid);
        if (entry && is_discarded_by_inode(event_type, entry->executable.mount_id, entry->executable.inode, 0)) {
            return 1;
        }
    }

    return 0;
}

void __attribute__((always_inline)) remove_pid_discarder(u32 tgid) {
    struct pid_discarder_t key = {
        .tgid = tgid,
    };

    remove_discarder(&pid_discarders, &key);
}

#endif
