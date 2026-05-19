
# -------- Options ----------

set(OCPN_TEST_REPO
    "opencpn/deeprey-alpha"
    CACHE STRING "Default repository for untagged builds"
)
set(OCPN_BETA_REPO
    "opencpn/deeprey-beta"
    CACHE STRING "Default repository for tagged builds matching 'beta'"
)
set(OCPN_RELEASE_REPO
    "opencpn/deeprey-prod"
    CACHE STRING "Default repository for tagged builds not matching 'beta'"
)
option(DEEPREY_USE_SVG "Use SVG graphics" ON)


#
#
# -------  Plugin setup --------
#
set(PKG_NAME deeprey-s63_pi)
set(PKG_VERSION  0.0.0.0)
set(PKG_PRERELEASE "")  # Empty, or a tag like 'beta'

set(DISPLAY_NAME "deeprey-s63")            # Dialogs, installer artifacts, ...
set(PLUGIN_API_NAME "Deeprey-S63")         # As of GetCommonName() in plugin API
set(PKG_SUMMARY "Deeprey S63 encrypted vector chart PlugIn")
set(PKG_DESCRIPTION [=[
Deeprey S63 PlugIn. Provides support for S63 encrypted vector charts and
exposes chart management to deeprey-gui through the shared DpS63API.
]=])


set(PKG_AUTHOR "BS")
set(PKG_IS_OPEN_SOURCE "yes")
set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/)
set(PKG_INFO_URL https://opencpn.org/OpenCPN/plugins/)

# ----------------------------------------------------------------------------#

#  Bundled GDAL CPL subset (kept in-repo: patched for the plugin).
set(SRC_CPL
    src/cpl/cpl_conv.cpp
    src/cpl/cpl_csv.cpp
    src/cpl/cpl_error.cpp
    src/cpl/cpl_findfile.cpp
    src/cpl/cpl_path.cpp
    src/cpl/cpl_string.cpp
    src/cpl/cpl_vsisimple.cpp
)

#  Bundled ISO 8211 reader (kept in-repo: S63-customised fork).
set(SRC_ISO8211
    src/myiso8211/ddffielddefn.cpp
    src/myiso8211/ddfmodule.cpp
    src/myiso8211/ddfrecord.cpp
    src/myiso8211/ddfsubfielddefn.cpp
    src/myiso8211/ddffield.cpp
    src/myiso8211/ddfutils.cpp
)

#  Bundled DSA signature verification (S63-specific crypto helpers).
set(SRC_DSA
    src/dsa/dsa_utils.cpp
    src/dsa/mp_math.c
    src/dsa/sha1.c
)

#  Bundled wxJSON (kept in-repo: s63 sources include the unprefixed headers).
set(SRC_JSON
    src/wxJSON/jsonreader.cpp
    src/wxJSON/jsonval.cpp
    src/wxJSON/jsonwriter.cpp
)

set(SRC_CORE
    src/s63_pi.cpp
    src/s63chart.cpp
    src/mygeom63.cpp
    src/TexFont.cpp
    src/InstallDirs.cpp
    src/DpS63API.cpp
    src/DpS63Identity.cpp
)

set(HEADERS_CORE
    src/s63_pi.h
    src/s63chart.h
    src/mygeom63.h
    src/TexFont.h
    src/InstallDirs.h
    src/bbox.h
    src/pi_s52s57.h
    src/triangulate.h
    src/DpS63Identity.h
    deeprey-api/s63/DpS63API.h
    deeprey-api/s63/DpS63Types.h
)

# -- Final aggregation
set(SRC
    ${SRC_CORE}
    ${SRC_CPL}
    ${SRC_ISO8211}
    ${SRC_DSA}
    ${SRC_JSON}
)

set(HEADERS
    ${HEADERS_CORE}
)

source_group("CPL"      FILES ${SRC_CPL})
source_group("ISO8211"  FILES ${SRC_ISO8211})
source_group("DSA"      FILES ${SRC_DSA})
source_group("wxJSON"   FILES ${SRC_JSON})
source_group("Source"   FILES ${SRC_CORE})
source_group("Headers"  FILES ${HEADERS_CORE})

add_definitions("-DocpnUSE_GL")

set(PKG_API_LIB api-16)  #  A directory in opencpn-libs/

macro(late_init)
  # Perform initialization after the PACKAGE_NAME library, compilers
  # and ocpn::api is available.
  if (DEEPREY_USE_SVG)
    target_compile_definitions(${PACKAGE_NAME} PUBLIC DEEPREY_USE_SVG)
  endif ()
endmacro ()

macro(add_plugin_libraries)
  # No external opencpn-libs required: the GDAL CPL subset, ISO 8211 reader,
  # DSA crypto and wxJSON are all bundled in-repo (see SRC_* above).
endmacro ()
