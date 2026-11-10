/*
 * K1801VM1 helper definitions
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

DEF_HELPER_2(raise_exception, noreturn, env, i32)
DEF_HELPER_1(wait, noreturn, env)
DEF_HELPER_1(sync_clock, void, env)
