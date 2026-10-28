# docs — render markdown spec & design docs to HTML/PDF via pandoc.
#
# Targets:
#   docs-html     all spec/design markdown → HTML
#   docs-pdf      all spec/design markdown → PDF (needs LaTeX)
#   docs          docs-html + docs-pdf
#
# Pandoc is detected at configure time; if missing, targets are
# generated as no-ops with a helpful message.

find_program(PANDOC_EXE pandoc)
if(NOT PANDOC_EXE)
    message(STATUS "docs: pandoc not found — install with:")
    message(STATUS "        macOS:    brew install pandoc")
    message(STATUS "        debian:   apt install pandoc")
    message(STATUS "      docs targets will print a hint when invoked")
    add_custom_target(docs-html
        COMMAND ${CMAKE_COMMAND} -E echo
            "pandoc not installed; cannot render docs.  See ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt for install commands.")
    add_custom_target(docs-pdf
        COMMAND ${CMAKE_COMMAND} -E echo
            "pandoc not installed; cannot render PDF.  See ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt for install commands.")
    add_custom_target(docs DEPENDS docs-html docs-pdf)
    return()
endif()

message(STATUS "docs: pandoc found at ${PANDOC_EXE}")

set(DOCS_OUT ${CMAKE_CURRENT_BINARY_DIR})
file(MAKE_DIRECTORY ${DOCS_OUT}/spec ${DOCS_OUT}/design)

# Collect every markdown source under spec/ and design/.
file(GLOB SPEC_MDS    ${CMAKE_SOURCE_DIR}/spec/*.md)
file(GLOB DESIGN_MDS  ${CMAKE_SOURCE_DIR}/design/*.md)

set(HTML_OUTPUTS)
set(PDF_OUTPUTS)

foreach(SRC IN LISTS SPEC_MDS DESIGN_MDS)
    get_filename_component(BASENAME ${SRC} NAME_WE)
    file(RELATIVE_PATH RELDIR ${CMAKE_SOURCE_DIR} ${SRC})
    get_filename_component(SUBDIR ${RELDIR} DIRECTORY)

    set(HTML_OUT ${DOCS_OUT}/${SUBDIR}/${BASENAME}.html)
    set(PDF_OUT  ${DOCS_OUT}/${SUBDIR}/${BASENAME}.pdf)

    add_custom_command(
        OUTPUT ${HTML_OUT}
        COMMAND ${PANDOC_EXE}
            --from gfm
            --to   html5
            --standalone
            --toc
            --metadata title=${BASENAME}
            --output  ${HTML_OUT}
            ${SRC}
        DEPENDS ${SRC}
        COMMENT "pandoc ${SUBDIR}/${BASENAME}.md → html"
        VERBATIM)
    list(APPEND HTML_OUTPUTS ${HTML_OUT})

    # PDF only if a LaTeX engine is available.
    add_custom_command(
        OUTPUT ${PDF_OUT}
        COMMAND ${PANDOC_EXE}
            --from gfm
            --to   pdf
            --pdf-engine=xelatex
            --metadata title=${BASENAME}
            --output  ${PDF_OUT}
            ${SRC}
        DEPENDS ${SRC}
        COMMENT "pandoc ${SUBDIR}/${BASENAME}.md → pdf"
        VERBATIM)
    list(APPEND PDF_OUTPUTS ${PDF_OUT})
endforeach()

# Top-level README.md & REPORT.md too.
foreach(EXTRA README REPORT)
    set(SRC ${CMAKE_SOURCE_DIR}/${EXTRA}.md)
    if(EXISTS ${SRC})
        set(HTML_OUT ${DOCS_OUT}/${EXTRA}.html)
        add_custom_command(
            OUTPUT ${HTML_OUT}
            COMMAND ${PANDOC_EXE}
                --from gfm --to html5 --standalone --toc
                --metadata title=${EXTRA}
                --output  ${HTML_OUT}
                ${SRC}
            DEPENDS ${SRC}
            COMMENT "pandoc ${EXTRA}.md → html"
            VERBATIM)
        list(APPEND HTML_OUTPUTS ${HTML_OUT})
    endif()
endforeach()

# Index page linking everything.
set(INDEX_OUT ${DOCS_OUT}/index.html)
add_custom_command(
    OUTPUT ${INDEX_OUT}
    COMMAND ${CMAKE_COMMAND}
        -DDOCS_OUT=${DOCS_OUT}
        -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/make_index.cmake
    DEPENDS ${HTML_OUTPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/make_index.cmake
    COMMENT "Generating docs/index.html"
    VERBATIM)
list(APPEND HTML_OUTPUTS ${INDEX_OUT})

add_custom_target(docs-html DEPENDS ${HTML_OUTPUTS})
add_custom_target(docs-pdf  DEPENDS ${PDF_OUTPUTS})
add_custom_target(docs      DEPENDS docs-html docs-pdf)

message(STATUS "docs: outputs will be written to ${DOCS_OUT}")
