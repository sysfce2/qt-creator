add_qtc_test(tst_manual_widgets_styledbar
  MANUALTEST
  DEPENDS Qt::Gui Utils Core
  SOURCES
    tst_manual_widgets_styledbar.cpp
    ../common/themeselector.cpp ../common/themeselector.h
    ../common/themes.qrc
)
