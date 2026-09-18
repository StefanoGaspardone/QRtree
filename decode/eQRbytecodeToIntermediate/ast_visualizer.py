import os
from graphviz import Digraph


class ASTNode:
    """
    Rappresenta un nodo dell'AST. Memorizza il nome del simbolo/token
    e la lista dei nodi figli.
    """
    def __init__(self, label: str, children: list | None = None):
        self.label = str(label)
        self.children = children if children is not None else []

    def add_child(self, child):
        if child is not None:
            self.children.append(child)


def render_ast(ast_root: ASTNode, output_path: str) -> None:
    """
    Costruisce e genera l'immagine PNG del parse tree a partire dalla radice ASTNode.
    """
    if ast_root is None:
        return

    try:
        dot = Digraph(comment='AST Parse Tree')
        node_counter = [0]

        def _traverse(node, parent_id=None):
            current_id = str(node_counter[0])
            node_counter[0] += 1

            # Mostra solo il tipo / etichetta del nodo
            dot.node(current_id, str(node.label))

            if parent_id is not None:
                dot.edge(parent_id, current_id)

            for child in node.children:
                if isinstance(child, ASTNode):
                    _traverse(child, current_id)
                elif child is not None:
                    # Per foglie o valori primitivi rimasti
                    child_id = str(node_counter[0])
                    node_counter[0] += 1
                    dot.node(child_id, str(child))
                    dot.edge(current_id, child_id)

        _traverse(ast_root)
        dot.render(output_path, format="png", cleanup=True)
        print(f"Albero sintattico salvato in: {output_path}.png")

    except Exception as e:
        print(f"Impossibile generare l'immagine dell'AST: {e}")