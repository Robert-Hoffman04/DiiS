#include "gx_frameplan.h"

void GxDirtyBitmap::init(u32 regionSize, u32 pageSize)
{
	m_pageSize = (pageSize == 0) ? 1 : pageSize;
	u32 pages = (regionSize + m_pageSize - 1) / m_pageSize;
	m_pageCount = (pages > kMaxPages) ? 0 : pages; // see header: safe no-op degrade
	clear();
}

void GxDirtyBitmap::clear()
{
	for (u32 i = 0; i < kMaxPages / 32; ++i)
		m_bits[i] = 0;
	m_anyDirty = false;
}

void GxDirtyBitmap::markRange(u32 offset, u32 size)
{
	if (size == 0 || m_pageCount == 0)
		return;
	u32 firstPage = offset / m_pageSize;
	u32 lastPage = (offset + size - 1) / m_pageSize;
	if (firstPage >= m_pageCount)
		return;
	if (lastPage >= m_pageCount)
		lastPage = m_pageCount - 1;
	for (u32 p = firstPage; p <= lastPage; ++p)
		m_bits[p / 32] |= (1u << (p % 32));
	m_anyDirty = true;
}

bool GxDirtyBitmap::isPageDirty(u32 pageIndex) const
{
	if (pageIndex >= m_pageCount)
		return false;
	return (m_bits[pageIndex / 32] & (1u << (pageIndex % 32))) != 0;
}

void GxBandTracker::beginFrame()
{
	m_count = 0;
}

void GxBandTracker::recordChangeAtLine(int line)
{
	if (line <= 0)
		return;
	// Dedupe consecutive same-line calls cheaply (the overwhelmingly
	// common case: several registers touched in the same HBlank) without
	// a full scan; an occasional duplicate elsewhere in the array is
	// harmless (finalize() sorts + naturally collapses it into the same
	// band boundary).
	if (m_count > 0 && m_boundaries[m_count - 1] == line)
		return;
	if (m_count >= kMaxBoundaries)
		return; // degrade: an extremely effects-heavy frame just merges
		        // its excess boundaries into the last recorded band rather
		        // than overrunning the array - see finalize()'s cap note.
	m_boundaries[m_count++] = line;
}

int GxBandTracker::finalize(int totalLines, int outStarts[], int outEnds[]) const
{
	// Sort the (typically tiny - single digits to low tens) boundary set.
	// Insertion sort is the right tool here: kMaxBoundaries is small and
	// this runs once per frame, not per scanline.
	int sorted[kMaxBoundaries];
	int n = m_count;
	for (int i = 0; i < n; ++i) sorted[i] = m_boundaries[i];
	for (int i = 1; i < n; ++i) {
		int v = sorted[i], j = i - 1;
		while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
		sorted[j + 1] = v;
	}

	int bandCount = 0;
	int start = 0;
	for (int i = 0; i < n; ++i) {
		if (sorted[i] <= start || sorted[i] >= totalLines)
			continue; // dedupe / out-of-range guard
		outStarts[bandCount] = start;
		outEnds[bandCount] = sorted[i];
		++bandCount;
		start = sorted[i];
	}
	outStarts[bandCount] = start;
	outEnds[bandCount] = totalLines;
	++bandCount;
	return bandCount;
}

void GxFramePlan::beginFrame()
{
	vram.clear();
	palette.clear();
	oam.clear();
	bands.beginFrame();
	for (int i = 0; i < GXHOT_COUNT; ++i)
		m_hot[i] = false;
}
