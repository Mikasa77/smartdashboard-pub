/*
 * Copyright (C) 2026 Peter Jones
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "tab_registry.h"
#include "dashboard_log.h"

static const char *TAG = "tab_registry";

static tab_entry_t s_tabs[MAX_TABS];
static int         s_count = 0;

void tab_register(const char *id, const char *label, tab_init_fn_t init_fn)
{
    if (s_count >= MAX_TABS) {
        LOG_W(TAG, "Registry full — ignoring tab '%s'", id);
        return;
    }
    s_tabs[s_count].id      = id;
    s_tabs[s_count].label   = label;
    s_tabs[s_count].init_fn = init_fn;
    s_count++;
    LOG_D(TAG, "Registered '%s' (%d total)", label, s_count);
}

const tab_entry_t *tab_get_all(void)  { return s_tabs; }
int                tab_get_count(void) { return s_count; }
