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

from .myScanner import Scanner
from .myParser import Parser
import os

def encode(file, debug, languages = None, max_depth = 1):
    fileName = os.path.splitext(file)[0]
    scanner = Scanner(debug)
    parser_obj = Parser(scanner, fileName, debug)
 
    with open(f"{fileName}.qr", 'r', encoding = 'utf-8') as input_file:
        source_text = input_file.read()
 
    parser_obj.encode(source_text, languages = languages, exh_max_depth = max_depth)