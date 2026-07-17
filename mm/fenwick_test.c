#include <kunit/test.h>
#include "fenwick.h"

static void do_tests(struct kunit *test)
{
    fenwick_tree ft;
    fenwick_init(&ft, 10);

    //test that array allocates 10 items plus 1 for index 0
    KUNIT_EXPECT_EQ(test, sizeof(ft.array)/sizeof(ft.array[0]), 11);

    //add (in order) [2,1,4,5,2]
    fenwick_add_new(&ft, 2);
    fenwick_add_new(&ft, 1);
    fenwick_add_new(&ft, 4);
    fenwick_add_new(&ft, 5);
    fenwick_add_new(&ft, 2);

    //check depths
    KUNIT_EXPECT_EQ(test, fenwick_get_depth(&ft, 5), 0);
    KUNIT_EXPECT_EQ(test, fenwick_get_depth(&ft, 3), 7);
    KUNIT_EXPECT_EQ(test, fenwick_get_depth(&ft, 1), 12);

    //evict the 1, move 4 to the back
    fenwick_remove(&ft, 2, 1);
    fenwick_move_to_back(&ft, 3, 4);

    //check depths, list is now [2,_,_,5,2,4]
    KUNIT_EXPECT_EQ(test, fenwick_get_depth(&ft, 1), 11);
    KUNIT_EXPECT_EQ(test, fenwick_get_depth(&ft, 3), 11);
    KUNIT_EXPECT_EQ(test, fenwick_get_depth(&ft, 5), 4);

    fenwick_free(&ft);
    KUNIT_EXPECT_EQ(test, ft.array, NULL);
}