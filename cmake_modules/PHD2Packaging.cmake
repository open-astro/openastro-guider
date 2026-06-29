#=============================================================================
# Copyright 2017, Max Planck Society.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software without
#    specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
# OF THE POSSIBILITY OF SUCH DAMAGE.

# File created by Raffi Enficiaud
#=============================================================================



# Install rules for the Linux build. The installable .deb is produced by
# debian/rules (dpkg-buildpackage, via build-deb.sh); these install() commands
# are what dh_auto_install runs. The legacy CPack NSIS (Windows) and CPack DEB
# paths were removed — cpack is never invoked in this project.
if(UNIX AND NOT APPLE)
  install(TARGETS phd2
          RUNTIME DESTINATION bin)
  configure_file(phd2.sh.in phd2.sh @ONLY)
  install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/phd2.sh
          DESTINATION bin
          RENAME phd2)
  install(FILES ${PHD_INSTALL_LIBS}
          DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/phd2/)
  # Icon, .desktop launcher, and AppStream metainfo are installed under the
  # openastro-guider name so the desktop file's Icon=openastro-guider resolves
  # via XDG icon theme lookup, GNOME Software / KDE Discover index us under
  # the new app id, and `update-desktop-database` registers the right
  # Exec=openastro-guider mapping. These must match the renamed binary
  # (/usr/bin/openastro-guider), otherwise the desktop entry would fail at
  # runtime.
  install(FILES ${PHD_PROJECT_ROOT_DIR}/icons/phd2_48.png
          DESTINATION ${CMAKE_INSTALL_PREFIX}/share/pixmaps/
          RENAME "openastro-guider.png")
  install(FILES ${PHD_PROJECT_ROOT_DIR}/openastro-guider.desktop
          DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications/ )
  install(FILES ${PHD_PROJECT_ROOT_DIR}/openastro-guider.appdata.xml
          DESTINATION ${CMAKE_INSTALL_PREFIX}/share/metainfo/ )
endif()
