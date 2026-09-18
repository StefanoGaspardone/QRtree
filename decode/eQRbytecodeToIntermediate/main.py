############################################################################################################################
# QRtree Software
# This software is released under the GNU General Public License v3.0.
#
# The QRtree software is an implementation of the QRtree dialect, that allows the embedding of decision trees in a QR code.
# For more information, read the file README.md.
#
# Please, cite this software (even if you just use a part of it) as:
# S. Scanzio, M. Rosani, and M. Scamuzzi, “QRtree software,” GitHub. [Online]. Available: https://github.com/eQR-code/QRtree
############################################################################################################################

import os
from .myParser import Parser
from .myScanner import Scanner
from .ast_visualizer import render_ast


def decode(file: str, debug: bool = False, generate_image: bool | None = None):
    if generate_image is None:
        generate_image = debug

    fileName = os.path.splitext(file)[0]
    scanner = Scanner(debug)
    parser_obj = Parser(scanner, fileName, debug)

    yacc_parser = parser_obj.parser

    with open(f"{fileName}.bin") as input_file:
        ast_root = yacc_parser.parse(input_file.read())

    if generate_image and ast_root is not None:
        render_ast(ast_root, f"{fileName}_ast")