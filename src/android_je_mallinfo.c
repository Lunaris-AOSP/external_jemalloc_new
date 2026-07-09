/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

typedef ctl_arena_stats_t android_je_arena_stats_t;

static void
android_je_read_arena_stats(arena_t *arena, android_je_arena_stats_t *stats) {
	unsigned nthreads = 0;
	const char *dss = NULL;
	ssize_t dirty_decay_ms = 0;
	ssize_t muzzy_decay_ms = 0;
	size_t nactive = 0;
	size_t ndirty = 0;
	size_t nmuzzy = 0;

	memset(stats, 0, sizeof(*stats));
	arena_stats_merge(TSDN_NULL, arena, &nthreads, &dss, &dirty_decay_ms,
	    &muzzy_decay_ms, &nactive, &ndirty, &nmuzzy, &stats->astats,
	    stats->bstats, stats->lstats, stats->estats, &stats->hpastats);

	for (szind_t i = 0; i < SC_NBINS; i++) {
		bin_stats_t *bstats = &stats->bstats[i].stats_data;
		stats->allocated_small += bstats->curregs * sz_index2size(i);
	}
}

static arena_t *
android_je_get_arena(size_t aidx) {
	if (aidx >= narenas_auto) {
		return NULL;
	}
	return arena_get(TSDN_NULL, (unsigned)aidx, false);
}

struct mallinfo
je_mallinfo(void) {
	struct mallinfo mi;
	memset(&mi, 0, sizeof(mi));

	for (unsigned i = 0; i < narenas_auto; i++) {
		arena_t *arena = arena_get(TSDN_NULL, i, false);
		if (arena == NULL) {
			continue;
		}
		android_je_arena_stats_t stats;
		android_je_read_arena_stats(arena, &stats);
		mi.hblkhd += stats.astats.mapped;
		mi.uordblks += stats.allocated_small + stats.astats.allocated_large;
	}
	if (mi.hblkhd >= mi.uordblks) {
		mi.fordblks = mi.hblkhd - mi.uordblks;
	}
	mi.usmblks = mi.hblkhd;
	return mi;
}

size_t
je_mallinfo_narenas(void) {
	return narenas_auto;
}

size_t
je_mallinfo_nbins(void) {
	return SC_NBINS;
}

struct mallinfo
je_mallinfo_arena_info(size_t aidx) {
	struct mallinfo mi;
	memset(&mi, 0, sizeof(mi));

	arena_t *arena = android_je_get_arena(aidx);
	if (arena == NULL) {
		return mi;
	}

	android_je_arena_stats_t stats;
	android_je_read_arena_stats(arena, &stats);
	mi.hblkhd = stats.astats.mapped;
	mi.ordblks = stats.astats.allocated_large;
	mi.fsmblks = stats.allocated_small;
	return mi;
}

struct mallinfo
je_mallinfo_bin_info(size_t aidx, size_t bidx) {
	struct mallinfo mi;
	memset(&mi, 0, sizeof(mi));

	arena_t *arena = android_je_get_arena(aidx);
	if (arena == NULL || bidx >= SC_NBINS) {
		return mi;
	}

	android_je_arena_stats_t stats;
	android_je_read_arena_stats(arena, &stats);
	bin_stats_t *bstats = &stats.bstats[bidx].stats_data;
	mi.ordblks = bin_infos[bidx].reg_size * bstats->curregs;
	mi.uordblks = (size_t)bstats->nmalloc;
	mi.fordblks = (size_t)bstats->ndalloc;
	return mi;
}
