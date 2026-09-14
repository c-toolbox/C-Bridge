if(WIN32 AND CBRIDGE_INSTALL_DEPENDENCIES)
# Runtime data staged next to the executable. Kept as a directory install so
# configs can be edited in place on deployed machines.
install(DIRECTORY configs
  DESTINATION "${KDE_INSTALL_BINDIR}/data"
  PATTERN ".git*" EXCLUDE
)

# Breeze color scheme, mirroring C-Slice's data install: KColorSchemeManager finds
# <appdir>/data/color-schemes at runtime because QStandardPaths::GenericDataLocation
# on Windows includes the application directory and its data folder.
install(DIRECTORY color-schemes
  DESTINATION "${KDE_INSTALL_BINDIR}/data"
)

# Qt's breeze icons (cursors etc.), same as C-Slice's "Copying Breeze Icons" step.
get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
install(DIRECTORY "${_qt_bin_dir}/data/icons" DESTINATION "${KDE_INSTALL_BINDIR}/data" OPTIONAL)

# Application icons, same as C-Slice's "Copying Data Items" step.
install(FILES images/C_transparent.ico images/C_transparent.png images/icon.rc
  DESTINATION "${KDE_INSTALL_BINDIR}/data/images")

install(DIRECTORY DESTINATION "${KDE_INSTALL_BINDIR}/data/log")
endif()