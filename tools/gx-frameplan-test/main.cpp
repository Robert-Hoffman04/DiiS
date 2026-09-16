// Host-side unit tests for source/gx/gx_frameplan.* (Stage 0 - frame
// analysis / draw plan). Pure logic, no NDS/GBA/GX dependency.
//
// Build: g++ -std=gnu++17 -I../../source main.cpp ../../source/gx/gx_frameplan.cpp
//            -o gx_frameplan_test
// Run:   ./gx_frameplan_test

#include <cstdio>
#include "gx/gx_frameplan.h"

static int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		++g_failures; \
	} \
} while (0)

static void test_dirty_bitmap_marks_correct_pages()
{
	GxDirtyBitmap bm;
	bm.init(1024, 32); // 32 pages of 32 bytes
	CHECK(bm.pageCount() == 32);
	CHECK(!bm.anyDirty());

	bm.markRange(40, 10); // spans page 1 (32-63)
	CHECK(bm.anyDirty());
	CHECK(!bm.isPageDirty(0));
	CHECK(bm.isPageDirty(1));
	CHECK(!bm.isPageDirty(2));
}

static void test_dirty_bitmap_range_spans_multiple_pages()
{
	GxDirtyBitmap bm;
	bm.init(1024, 32);
	bm.markRange(20, 50); // bytes 20..69 -> pages 0,1,2
	CHECK(bm.isPageDirty(0));
	CHECK(bm.isPageDirty(1));
	CHECK(bm.isPageDirty(2));
	CHECK(!bm.isPageDirty(3));
}

static void test_dirty_bitmap_clamps_out_of_range()
{
	GxDirtyBitmap bm;
	bm.init(1024, 32);
	bm.markRange(1000, 1000); // runs off the end
	CHECK(bm.isPageDirty(31)); // last page still gets marked, no crash/UB
}

static void test_dirty_bitmap_clear_resets()
{
	GxDirtyBitmap bm;
	bm.init(1024, 32);
	bm.markRange(0, 4);
	CHECK(bm.anyDirty());
	bm.clear();
	CHECK(!bm.anyDirty());
	CHECK(!bm.isPageDirty(0));
}

static void test_dirty_bitmap_oversized_region_is_safe_noop()
{
	GxDirtyBitmap bm;
	bm.init(1u << 30, 1); // would need > kMaxPages pages -> degrades to a no-op tracker
	CHECK(bm.pageCount() == 0);
	bm.markRange(0, 100);
	CHECK(!bm.anyDirty()); // must not overrun m_bits
}

static void test_band_tracker_no_writes_is_one_band()
{
	GxBandTracker bt;
	bt.beginFrame();
	int starts[GxBandTracker::kMaxBands], ends[GxBandTracker::kMaxBands];
	int n = bt.finalize(160, starts, ends);
	CHECK(n == 1);
	CHECK(starts[0] == 0 && ends[0] == 160);
}

static void test_band_tracker_single_midframe_change()
{
	GxBandTracker bt;
	bt.beginFrame();
	bt.recordChangeAtLine(80);
	int starts[GxBandTracker::kMaxBands], ends[GxBandTracker::kMaxBands];
	int n = bt.finalize(160, starts, ends);
	CHECK(n == 2);
	CHECK(starts[0] == 0 && ends[0] == 80);
	CHECK(starts[1] == 80 && ends[1] == 160);
}

static void test_band_tracker_ignores_line_zero()
{
	GxBandTracker bt;
	bt.beginFrame();
	bt.recordChangeAtLine(0); // frame start is always band 0's start already
	int starts[GxBandTracker::kMaxBands], ends[GxBandTracker::kMaxBands];
	int n = bt.finalize(160, starts, ends);
	CHECK(n == 1);
}

static void test_band_tracker_dedupes_and_sorts_out_of_order_writes()
{
	GxBandTracker bt;
	bt.beginFrame();
	bt.recordChangeAtLine(100);
	bt.recordChangeAtLine(40);
	bt.recordChangeAtLine(100); // duplicate, non-adjacent call
	bt.recordChangeAtLine(40);  // duplicate
	int starts[GxBandTracker::kMaxBands], ends[GxBandTracker::kMaxBands];
	int n = bt.finalize(160, starts, ends);
	CHECK(n == 3);
	CHECK(starts[0] == 0   && ends[0] == 40);
	CHECK(starts[1] == 40  && ends[1] == 100);
	CHECK(starts[2] == 100 && ends[2] == 160);
}

static void test_band_tracker_ignores_out_of_range_line()
{
	GxBandTracker bt;
	bt.beginFrame();
	bt.recordChangeAtLine(160); // == totalLines, not a valid interior boundary
	bt.recordChangeAtLine(500); // way out of range
	int starts[GxBandTracker::kMaxBands], ends[GxBandTracker::kMaxBands];
	int n = bt.finalize(160, starts, ends);
	CHECK(n == 1);
}

static void test_band_tracker_begin_frame_resets()
{
	GxBandTracker bt;
	bt.beginFrame();
	bt.recordChangeAtLine(50);
	CHECK(bt.boundaryCount() == 1);
	bt.beginFrame();
	CHECK(bt.boundaryCount() == 0);
	int starts[GxBandTracker::kMaxBands], ends[GxBandTracker::kMaxBands];
	int n = bt.finalize(160, starts, ends);
	CHECK(n == 1); // stale boundary from the previous frame must not leak through
}

static void test_frameplan_begin_frame_clears_everything()
{
	GxFramePlan plan;
	plan.vram.init(1024, 32);
	plan.palette.init(64, 32);
	plan.oam.init(64, 32);

	plan.vram.markRange(0, 4);
	plan.palette.markRange(0, 4);
	plan.bands.recordChangeAtLine(10);
	plan.setHot(GXHOT_MOSAIC, true);

	CHECK(plan.vram.anyDirty());
	CHECK(plan.isHot(GXHOT_MOSAIC));

	plan.beginFrame();

	CHECK(!plan.vram.anyDirty());
	CHECK(!plan.palette.anyDirty());
	CHECK(!plan.oam.anyDirty());
	CHECK(plan.bands.boundaryCount() == 0);
	CHECK(!plan.isHot(GXHOT_MOSAIC));
	CHECK(!plan.isHot(GXHOT_SHADOW)); // untouched flags also read back false
}

int main()
{
	test_dirty_bitmap_marks_correct_pages();
	test_dirty_bitmap_range_spans_multiple_pages();
	test_dirty_bitmap_clamps_out_of_range();
	test_dirty_bitmap_clear_resets();
	test_dirty_bitmap_oversized_region_is_safe_noop();

	test_band_tracker_no_writes_is_one_band();
	test_band_tracker_single_midframe_change();
	test_band_tracker_ignores_line_zero();
	test_band_tracker_dedupes_and_sorts_out_of_order_writes();
	test_band_tracker_ignores_out_of_range_line();
	test_band_tracker_begin_frame_resets();

	test_frameplan_begin_frame_clears_everything();

	if (g_failures == 0) {
		printf("All tests passed.\n");
		return 0;
	}
	printf("%d test(s) failed.\n", g_failures);
	return 1;
}
