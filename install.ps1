# SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
# SPDX-License-Identifier: LGPL-2.1-or-later
# Wrapper so Windows users can run .\install.ps1 from the repo root.
& (Join-Path $PSScriptRoot "packaging\windows\install.ps1") @args
