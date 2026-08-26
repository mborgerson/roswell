/*
 * ExfInterlockedInsertHeadList / ExfInterlockedInsertTailList /
 * ExfInterlockedRemoveHeadList -- the Xbox spinlock-free interlocked list
 * primitives.  The ordinals were unmapped bugcheck stubs; the kernel now
 * carries thin FASTCALL adapters (xb/exflist.c) that splice with
 * interrupts masked and return the neighbour that held the slot before
 * the operation (NULL when the list was empty), matching retail.
 */

#include "../harness.h"

static void init_head(LIST_ENTRY *h)
{
    h->Flink = h;
    h->Blink = h;
}

static bool t_insert_head(void)
{
    LIST_ENTRY head, a, b;
    PLIST_ENTRY prev;

    init_head(&head);

    /* First insert into an empty list returns NULL. */
    prev = ExfInterlockedInsertHeadList(&head, &a);
    ASSERT_TRUE(prev == NULL);
    ASSERT_TRUE(head.Flink == &a);
    ASSERT_TRUE(head.Blink == &a);
    ASSERT_TRUE(a.Flink == &head);
    ASSERT_TRUE(a.Blink == &head);

    /* Second head insert returns the previous first entry (a). */
    prev = ExfInterlockedInsertHeadList(&head, &b);
    ASSERT_TRUE(prev == &a);
    ASSERT_TRUE(head.Flink == &b);
    ASSERT_TRUE(b.Flink == &a);
    ASSERT_TRUE(a.Blink == &b);
    return true;
}

static bool t_insert_tail(void)
{
    LIST_ENTRY head, a, b;
    PLIST_ENTRY prev;

    init_head(&head);

    /* First tail insert into an empty list returns NULL. */
    prev = ExfInterlockedInsertTailList(&head, &a);
    ASSERT_TRUE(prev == NULL);
    ASSERT_TRUE(head.Blink == &a);
    ASSERT_TRUE(head.Flink == &a);

    /* Second tail insert returns the previous last entry (a). */
    prev = ExfInterlockedInsertTailList(&head, &b);
    ASSERT_TRUE(prev == &a);
    ASSERT_TRUE(head.Blink == &b);
    ASSERT_TRUE(a.Flink == &b);
    ASSERT_TRUE(b.Flink == &head);
    return true;
}

static bool t_remove_head(void)
{
    LIST_ENTRY head, a, b, c;
    PLIST_ENTRY got;

    init_head(&head);

    /* Remove from an empty list returns NULL. */
    got = ExfInterlockedRemoveHeadList(&head);
    ASSERT_TRUE(got == NULL);

    /* Build [a, b, c] via tail inserts, then drain from the head in order. */
    ExfInterlockedInsertTailList(&head, &a);
    ExfInterlockedInsertTailList(&head, &b);
    ExfInterlockedInsertTailList(&head, &c);

    got = ExfInterlockedRemoveHeadList(&head);
    ASSERT_TRUE(got == &a);
    got = ExfInterlockedRemoveHeadList(&head);
    ASSERT_TRUE(got == &b);
    got = ExfInterlockedRemoveHeadList(&head);
    ASSERT_TRUE(got == &c);

    /* List is empty again. */
    ASSERT_TRUE(head.Flink == &head);
    ASSERT_TRUE(head.Blink == &head);
    got = ExfInterlockedRemoveHeadList(&head);
    ASSERT_TRUE(got == NULL);
    return true;
}

static const test_entry_t ex_list_entries[] = {
    {"insert_head", t_insert_head},
    {"insert_tail", t_insert_tail},
    {"remove_head", t_remove_head},
};

DEFINE_GROUP(ex_list, "ex/list");
