/* SPDX-License-Identifier: MIT
 * Unit fixture for the actual driver helper, with synthetic PTE boundaries.
 * This fixture does not execute a kernel or establish guest runtime evidence.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

typedef uint64_t u64;
typedef struct {
    unsigned long pfn;
    unsigned rights;
    bool valid, software_write;
} pte_t;
typedef unsigned spinlock_t;
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define VM_PFNMAP 0x400UL
#define O_SYNC 0x101000
#define U64_MAX UINT64_MAX
#define PFN_PHYS(pfn) ((u64)(pfn) << PAGE_SHIFT)
#define offset_in_page(address) ((address) & (PAGE_SIZE - 1))
struct file { int f_flags; u64 mem_id; };
struct vm_area_struct {
    unsigned long vm_start, vm_end, vm_flags;
    struct file *vm_file;
    void *vm_mm;
};
static pte_t entries[4];
static spinlock_t lock;
static unsigned follows, unlocks;
static int fail_page;
static unsigned long page_size;
static struct file file;
static struct vm_area_struct vma;

static bool obmm_file_matches_region(struct file *candidate, u64 mem_id)
{
    return candidate == &file && mem_id == file.mem_id;
}
static unsigned long vma_kernel_pagesize(struct vm_area_struct *area)
{
    assert(area == &vma);
    return page_size;
}
static int follow_pte(void *mm, unsigned long page, pte_t **out, spinlock_t **ptl)
{
    assert(mm == &vma && page % PAGE_SIZE == 0 && !lock);
    unsigned index = (page - vma.vm_start) / PAGE_SIZE;
    assert(index < 4);
    ++follows;
    if ((int)index == fail_page) return -EIO;
    lock = 1;
    *out = &entries[index];
    *ptl = &lock;
    return 0;
}
static pte_t ptep_get(pte_t *entry) { assert(lock); return *entry; }
static unsigned long pte_pfn(pte_t entry) { assert(lock); return entry.pfn; }
static bool pte_access_permitted(pte_t entry, bool write)
{
    assert(lock);
    return entry.valid && (entry.rights & 1) && (!write || entry.software_write);
}
static bool pte_rdonly(pte_t entry) { assert(lock); return !(entry.rights & 2); }
static void pte_unmap_unlock(pte_t *entry, spinlock_t *ptl)
{
    assert(entry >= entries && entry < entries + 4 && ptl == &lock && lock);
    lock = 0;
    ++unlocks;
}

#include "strict_pto_view.inc"

static void reset(void)
{
    assert(!lock);
    follows = unlocks = 0;
    fail_page = -1;
    page_size = PAGE_SIZE;
    file = (struct file) {.f_flags = O_SYNC, .mem_id = 7};
    vma = (struct vm_area_struct) {
        .vm_start = 0x700000000000UL, .vm_end = 0x700000004000UL,
        .vm_flags = VM_PFNMAP, .vm_file = &file, .vm_mm = &vma,
    };
    for (unsigned i = 0; i < 4; ++i)
        entries[i] = (pte_t) {.pfn = 0x40000UL + i, .rights = 3, .valid = true};
}

static void rejected(int expected, u64 mem_id, u64 address, u64 length)
{
    u64 pa = 99, access = 99;
    assert(linqu_obmm_strict_pto_view(&vma, mem_id, address, length,
                                     &pa, &access) == expected);
    assert(pa == 99 && access == 99 && !lock);
}

int main(void)
{
    u64 pa, access;
    reset();
    /* Neither VMA nor Linux's software-write flags carry OBMM RW rights. */
    assert(!linqu_obmm_strict_pto_view(&vma, 7, vma.vm_start + 128,
                                      2 * PAGE_SIZE, &pa, &access));
    assert(pa == PFN_PHYS(0x40000) + 128 && access == 3);
    assert(follows == 3 && unlocks == 3);
    reset();
    entries[1].rights = 1;
    assert(!linqu_obmm_strict_pto_view(&vma, 7, vma.vm_start,
                                      3 * PAGE_SIZE, &pa, &access));
    assert(access == 1 && follows == 3 && unlocks == 3);
    reset();
    entries[1].rights = 1;
    entries[1].software_write = true;
    assert(!linqu_obmm_strict_pto_view(&vma, 7, vma.vm_start,
                                      2 * PAGE_SIZE, &pa, &access));
    assert(access == 1 && follows == 2 && unlocks == 2);
    reset();
    entries[2].rights = 0;
    rejected(-EACCES, 7, vma.vm_start, 3 * PAGE_SIZE);
    assert(follows == 3 && unlocks == 3);
    reset();
    entries[1].valid = false;
    rejected(-EACCES, 7, vma.vm_start, 2 * PAGE_SIZE);
    assert(follows == 2 && unlocks == 2);
    reset();
    entries[1].pfn += 2;
    rejected(-EINVAL, 7, vma.vm_start, 2 * PAGE_SIZE);
    assert(follows == 2 && unlocks == 2);
    reset();
    fail_page = 1;
    rejected(-EIO, 7, vma.vm_start, 2 * PAGE_SIZE);
    assert(follows == 2 && unlocks == 1);
    reset();
    rejected(-EFAULT, 8, vma.vm_start, PAGE_SIZE);
    assert(!follows);
    reset();
    vma.vm_flags = 0;
    rejected(-EFAULT, 7, vma.vm_start, PAGE_SIZE);
    assert(!follows);
    reset();
    file.f_flags = 0;
    rejected(-EOPNOTSUPP, 7, vma.vm_start, PAGE_SIZE);
    assert(!follows);
    reset();
    page_size = 2097152;
    rejected(-EOPNOTSUPP, 7, vma.vm_start, PAGE_SIZE);
    assert(!follows);
    reset();
    rejected(-EFAULT, 7, vma.vm_start, 0);
    rejected(-EFAULT, 7, vma.vm_end - 1, 2);
    rejected(-EFAULT, 7, U64_MAX - 1, 4);
    assert(!follows);
    puts("strict_pto_view_golden=pass cases=14");
    return 0;
}
