#include <sync/spin.h>
#include <utils/kheap.h>
#include <utils/kpanic.h>
#include <utils/kassert.h>
#include <utils/kprint.h>
#include <utils/kdump.h>
#include <lib/string.h>
#include <lib/stdtypes.h>
#include <kernel.h>
#include <messages.h>

struct kernel_heap_table_t kernel_heap_table;

static struct kernel_heap_entry_t *kheap_find_block(struct kernel_heap_entry_t *entry, bool is_valid, bool is_buzy);
static struct kernel_heap_entry_t *kheap_find_first_valid_block(struct kernel_heap_entry_t *entry);
static struct kernel_heap_entry_t *kheap_get_highest_entry();
static void kheap_validate();
static void kheap_test();

/*
 * Api - Kernel memory init
 */
extern void kheap_init()
{
    memset(&kernel_heap_table, 0, sizeof(struct kernel_heap_table_t));
    kheap_test();
}

/*
 * Api - Kernel memory alloc
 */
extern void *kmalloc(size_t size)
{
    struct kernel_heap_entry_t *current;

    /*
     * try to use existing suitable block
     */
    current = null;
    do
    {
        /* get free valid block */
        current = kheap_find_block(current, true, false);
        if (current != null)
        {
            /* check size is enough */
            if (current->size < size)
            {
                /* try to ask contribution from free left sibling */
                if (current->prev != null)
                {
                    /* sibling has found */
                    struct kernel_heap_entry_t *sibling = current->prev;
                    kassert(__FILE__, __LINE__, sibling->is_valid);
                    /* check whether sibling is free */
                    if (!sibling->is_buzy)
                    {
                        /* ask lack from left sibling */
                        size_t lack = size - current->size;
                        sibling->size -= lack;
                        current->addr -= lack;
                        current->size += lack;
                        kassert(__FILE__, __LINE__, current->size == size);
                        /* whether sibling is collapsed */
                        if (sibling->size == 0)
                        {
                            /* normalize pointers */
                            if (sibling->prev != null)
                            {
                                /* set new left sibling */
                                struct kernel_heap_entry_t *sibling2 = sibling->prev;
                                kassert(__FILE__, __LINE__, sibling2 != null);
                                sibling2->next = current;
                                current->prev = sibling2;
                            }
                            else
                            {
                                /* no left sibling anymore */
                                current->prev = null;
                            }
                            /* delete collapsed sibling */
                            memset(sibling, 0, sizeof(struct kernel_heap_entry_t));
                        }
                        /* occupy block */
                        current->is_buzy = true;
                        kassert(__FILE__, __LINE__, current->addr >= KHEAP_START_ADDR);
                        kassert(__FILE__, __LINE__, current->addr < KHEAP_END_ADDR);
                        kheap_validate();
                        return (void *)current->addr; /* suitable block has found */
                    }
                }
                else if (current->next == null)
                {
                    /* try to allocate new memory */
                    size_t heap_end_addr = current->addr + current->size;
                    size_t lack = size - current->size;
                    /* check free memory size is enought */
                    if (heap_end_addr + lack < KHEAP_END_ADDR)
                    {
                        /* occupy block */
                        current->size += lack;
                        current->is_buzy = true;
                        kassert(__FILE__, __LINE__, current->addr >= KHEAP_START_ADDR);
                        kassert(__FILE__, __LINE__, current->addr < KHEAP_END_ADDR);
                        kheap_validate();
                        return (void *)current->addr; /* suitable block has found */
                    }
                }
            }
            else
            {
                /* occupy block */
                current->is_buzy = true;
                /* try to contribute free right sibling */
                if (current->next != null)
                {
                    /* sibling has found */
                    struct kernel_heap_entry_t *sibling = current->next;
                    kassert(__FILE__, __LINE__, sibling->is_valid);
                    size_t surplus = current->size - size;
                    /* check whether sibling is free */
                    if (!sibling->is_buzy)
                    {
                        /* give surplus to right sibling */
                        current->size -= surplus;
                        sibling->addr -= surplus;
                        sibling->size += surplus;
                        kassert(__FILE__, __LINE__, current->size == size);
                    } else {
                        /* give surplus to new right sibling */
                        struct kernel_heap_entry_t *new_sibling;
                        new_sibling = kheap_find_block(null, false, false);
                        if (new_sibling != null) {
                          current->size -= surplus;
                          new_sibling->is_valid = true;
                          new_sibling->is_buzy = false;
                          new_sibling->addr = current->addr + current->size;
                          new_sibling->size = surplus;
                          new_sibling->prev = current;
                          new_sibling->next = current->next;
                          if (current->next) {
                            kassert(__FILE__, __LINE__, current->next->prev == current);
                            current->next->prev = new_sibling;
                          }
                          current->next = new_sibling;
                        }
                    }
                }
                kassert(__FILE__, __LINE__, current->addr >= KHEAP_START_ADDR);
                kassert(__FILE__, __LINE__, current->addr < KHEAP_END_ADDR);
                kheap_validate();
                return (void *)current->addr; /* suitable block has found */
            }
        }
    } while (current != null);

    /*
     * try to alloc new block
     */
    struct kernel_heap_entry_t *highest;
    size_t heap_end_addr = KHEAP_START_ADDR;
    /* find heap end address */
    highest = kheap_get_highest_entry();
    if (highest)
    {
        heap_end_addr = highest->addr + highest->size;
    }
    /* check free memory size is enought */
    if (heap_end_addr + size >= KHEAP_END_ADDR)
    {
        kpanic(MSG_KERNEL_HEAP_EXCEED);
    }
    /* get invalid block to occupy */
    current = null;
    current = kheap_find_block(current, false, false);
    if (current == null)
    {
        kpanic(MSG_KERNEL_HEAP_TABLE_EXCEED);
    }
    /* occupy block */
    current->addr = heap_end_addr;
    current->size = size;
    current->is_valid = true;
    current->is_buzy = true;
    /* normalize pointers */
    current->prev = highest;
    current->next = null;
    if (highest)
    {
        highest->next = current;
    }

    kassert(__FILE__, __LINE__, current->addr >= KHEAP_START_ADDR);
    kassert(__FILE__, __LINE__, current->addr < KHEAP_END_ADDR);
    kheap_validate();
    return (void *)current->addr;
}

/*
 * Api - Kernel memory free
 */
extern void kfree(void *addr)
{
    struct kernel_heap_entry_t *current;

    for (current = kheap_table_entries_start();
         current != kheap_table_entries_end();
         current = kheap_table_entries_next(current))
    {
        if (current->is_valid && (void *)current->addr == addr && current->is_buzy)
        {
            /* free block */
            current->is_buzy = false;
            /* try to merge with free left sibling */
            if (current->prev != null && !current->prev->is_buzy)
            {
                current->prev->size += current->size;
                /* delete collapsed block */
                memset(current, 0, sizeof(struct kernel_heap_entry_t));
            }
            /* try to merge with free right sibling */
            if (current->next != null && !current->next->is_buzy)
            {
                current->size += current->next->size;
                /* delete collapsed sibling */
                memset(current->next, 0, sizeof(struct kernel_heap_entry_t));
            }
            kheap_validate();
            return;
        }
    }

    kpanic("invalid kernel heap address to free %X", addr);
}

//
// List iteration support
//

/*
 * Api - Get first kernel heap table entry
 */
extern struct kernel_heap_entry_t *kheap_table_entries_start()
{
    return &kernel_heap_table.block[0];
}

/*
 * Api - Get next kernel heap table entry
 */
extern struct kernel_heap_entry_t *kheap_table_entries_next(struct kernel_heap_entry_t *entry)
{
    if (entry == null) {
      return kheap_table_entries_start();
    }

    for (int i = 0; i < KHEAP_MAX_ENTRIES; ++i)
    {
        if (&kernel_heap_table.block[i] == entry)
        {
            if (i + 1 < KHEAP_MAX_ENTRIES)
            {
                return &kernel_heap_table.block[i + 1];
            }
            else
            {
                return null;
            }
        }
    }

    return null;
}

/*
 * Api - Get last kernel heap table entry
 */
extern struct kernel_heap_entry_t *kheap_table_entries_end()
{
    return &kernel_heap_table.block[KHEAP_MAX_ENTRIES - 1];
}

//
// Table entries searching
//

/*
 * Find suitable block
 */
static struct kernel_heap_entry_t *kheap_find_block(struct kernel_heap_entry_t *entry, bool is_valid, bool is_buzy)
{
    /* lookup all entries */
    for (entry = kheap_table_entries_next(entry);
         entry != kheap_table_entries_end();
         entry = kheap_table_entries_next(entry))
    {
        /* whether block is suitable */
        if (entry->is_valid == is_valid && entry->is_buzy == is_buzy)
        {
            return entry;
        }
    }

    return null;
}

/*
 * Find first valid block
 */
static struct kernel_heap_entry_t *kheap_find_first_valid_block(struct kernel_heap_entry_t *entry)
{
    /* lookup all entries */
    for (entry = kheap_table_entries_next(entry);
         entry != kheap_table_entries_end();
         entry = kheap_table_entries_next(entry))
    {
        /* whether block is suitable */
        if (entry->is_valid == true)
        {
            return entry;
        }
    }

    return null;
}

/*
 * Find entry with highest memory address
 */
static struct kernel_heap_entry_t *kheap_get_highest_entry()
{
    struct kernel_heap_entry_t *entry;
    struct kernel_heap_entry_t *highest_entry = null;
    size_t max_right_border = 0;

    /* lookup all entries */
    for (entry = kheap_table_entries_start();
         entry != kheap_table_entries_end();
         entry = kheap_table_entries_next(entry))
    {
        /* whether block is valid */
        if (entry->is_valid == true)
        {
            size_t right_border = entry->addr + entry->size;
            if (right_border >= max_right_border)
            {
                /* update highest entry */
                max_right_border = right_border;
                highest_entry = entry;
            }
        }
    }

    return highest_entry;
}

/*
 * Check heap is valid
 */
static void kheap_validate()
{
#ifdef DEBUG
    struct kernel_heap_entry_t *entry;
    struct kernel_heap_entry_t *current;
    struct kernel_heap_entry_t *next;
    size_t addr;

    /* get first valid block */
    entry = kheap_find_first_valid_block(null);
    kassert(__FILE__, __LINE__, entry->prev == null);
    /* check no holes */
    current = entry;
    while (current)
    {
        addr = current->addr + current->size;
        next = current->next;
        current = current->next;
        if (next && next->addr != addr)
        {
            /* hole detected */
            kpanic("kernel heap corrupted");
        }
    }
#endif
}

/*
 * Smoke test
 */
static void kheap_test()
{
#ifdef TEST
    /* allocate 3 small blocks */
    void *addr1 = kmalloc(16);
    kassert(__FILE__, __LINE__, addr1 == (void *)KHEAP_START_ADDR);
    void *addr2 = kmalloc(16);
    kassert(__FILE__, __LINE__, addr2 == (void *)addr1 + 16);
    void *addr3 = kmalloc(16);
    kassert(__FILE__, __LINE__, addr3 == (void *)addr2 + 16);
    /* free middle block */
    kfree(addr2);
    /* allocate 2 tiny block */
    void *addr4 = kmalloc(8);
    kassert(__FILE__, __LINE__, addr4 == (void *)addr1 + 16);
    void *addr5 = kmalloc(6);
    kassert(__FILE__, __LINE__, addr5 == (void *)(addr4 + 8));
    /* allocate small block */
    void *addr6 = kmalloc(16);
    kassert(__FILE__, __LINE__, addr6 == (void *)(addr3 + 16));
    /* free first and last block */
    kfree(addr1);
    kfree(addr6);
    /* allocte 2 tiny block */
    void *addr7 = kmalloc(4);
    kdump_heap();
    kassert(__FILE__, __LINE__, addr7 == (void *)(addr1));
    void *addr8 = kmalloc(4);
    kdump_heap();
    kassert(__FILE__, __LINE__, addr8 == (void *)(addr1 + 4));
    /* allocate big block */
    void *addr9 = kmalloc(64);
    kassert(__FILE__, __LINE__, addr9 == (void *)(KHEAP_START_ADDR + 16 + 16));
    /* free all blocks */
    kfree(addr3);
    kfree(addr4);
    kfree(addr5);
    kfree(addr7);
    kfree(addr8);
    kfree(addr9);
    /* allocate 1 tiny block */
    void *addr10 = kmalloc(1);
    kassert(__FILE__, __LINE__, addr9 == (void *)KHEAP_START_ADDR);
    kfree(addr10);
    kprint("sss");
    /* clear heap table */
    memset(&kernel_heap_table, 0, sizeof(kernel_heap_table));
#endif
}
