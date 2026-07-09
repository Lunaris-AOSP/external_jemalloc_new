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

void
je_stats_arena(size_t arena_index, void (*callback)(size_t, size_t, size_t)) {
	arena_t *arena = android_je_get_arena(arena_index);
	if (arena == NULL) {
		return;
	}

	android_je_arena_stats_t stats;
	android_je_read_arena_stats(arena, &stats);

	for (szind_t i = 0; i < SC_NBINS; i++) {
		bin_stats_t *bstats = &stats.bstats[i].stats_data;
		callback(i, bin_infos[i].reg_size, bstats->curregs);
	}

	for (szind_t i = SC_NBINS; i < SC_NSIZES; i++) {
		arena_stats_large_t *lstats = &stats.lstats[i - SC_NBINS];
		callback(i, sz_index2size(i), lstats->curlextents);
	}
}
