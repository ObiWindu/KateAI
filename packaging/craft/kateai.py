# SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# KDE Craft blueprint. Copy this file into your Craft blueprints tree, e.g.
#   craft/etc/blueprints/locations/craft-blueprints-kde/extragear/kateai/kateai.py
# then:
#   craft kateai
# The plugin installs into Craft's Qt plugin dir, which Craft Kate already scans.

import info
from Package.CMakePackageBase import CMakePackageBase


class subinfo(info.infoclass):
    def setTargets(self):
        self.displayName = "Kate AI"
        self.description = "AI coding agent plugin for the Kate text editor"
        self.webpage = "https://github.com/KateAI/kate-ai"
        self.svnTargets["master"] = "https://github.com/KateAI/kate-ai.git"
        self.defaultTarget = "master"

    def setDependencies(self):
        self.buildDependencies["kde/frameworks/extra-cmake-modules"] = None
        self.runtimeDependencies["libs/qt6/qtbase"] = None
        self.runtimeDependencies["kde/frameworks/tier1/kcoreaddons"] = None
        self.runtimeDependencies["kde/frameworks/tier1/ki18n"] = None
        self.runtimeDependencies["kde/frameworks/tier1/kconfig"] = None
        self.runtimeDependencies["kde/frameworks/tier1/kwidgetsaddons"] = None
        self.runtimeDependencies["kde/frameworks/tier3/kconfigwidgets"] = None
        self.runtimeDependencies["kde/frameworks/tier3/kxmlgui"] = None
        self.runtimeDependencies["kde/frameworks/tier3/ktexteditor"] = None
        self.runtimeDependencies["kde/applications/kate"] = None


class Package(CMakePackageBase):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.subinfo.options.configure.args += [
            "-DKATEAI_USER_INSTALL=OFF",
            "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON",
        ]
