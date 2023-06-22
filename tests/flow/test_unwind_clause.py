from common import *
import re

redis_graph = None
GRAPH_ID = "unwind"

class testUnwindClause():
    
    def __init__(self):
        self.env = Env(decodeResponses=True)
        global redis_graph
        redis_con = self.env.getConnection()
        redis_graph = Graph(redis_con, GRAPH_ID)
 
    def test01_unwind_null(self):
        query = """UNWIND null AS x RETURN x"""
        actual_result = redis_graph.query(query)
        expected = []
        self.env.assertEqual(actual_result.result_set, expected)

    def test02_unwind_input_types(self):
        # map list input
        query = """UNWIND ([{x:3, y:5}]) AS q RETURN q"""
        actual_result = redis_graph.query(query)
        expected = [[{'x':3, 'y':5}]]
        self.env.assertEqual(actual_result.result_set, expected)

        # map input
        query = """UNWIND ({x:3, y:5}) AS q RETURN q"""
        actual_result = redis_graph.query(query)
        expected = [[{'x': 3, 'y': 5}]]
        self.env.assertEqual(actual_result.result_set, expected)

        # map containing a key with the value NULL
        query = """UNWIND ({x:null}) AS q RETURN q"""
        actual_result = redis_graph.query(query)
        expected = [[{'x': None}]]
        self.env.assertEqual(actual_result.result_set, expected)

        # integer input
        query = """UNWIND 5 AS q RETURN q"""
        actual_result = redis_graph.query(query)
        expected = [[5]]
        self.env.assertEqual(actual_result.result_set, expected)

        # string input
        query = """UNWIND 'abc' AS q RETURN q"""
        actual_result = redis_graph.query(query)
        expected = [['abc']]
        self.env.assertEqual(actual_result.result_set, expected)

        # floating-point input
        query = """UNWIND 7.5 AS q RETURN q"""
        actual_result = redis_graph.query(query)
        expected = [[7.5]]
        self.env.assertEqual(actual_result.result_set, expected)

        # nested list
        query = """WITH [[1, 2], [3, 4], 5] AS nested UNWIND nested AS x RETURN x"""
        actual_result = redis_graph.query(query)
        expected = [[[1, 2]], [[3, 4]], [5]]
        self.env.assertEqual(actual_result.result_set, expected)

        # nested list double unwind
        query = """WITH [[1, 2], [3, 4], 5] AS nested UNWIND nested AS x UNWIND x AS y RETURN y"""
        actual_result = redis_graph.query(query)
        expected = [[1], [2], [3], [4], [5]]
        self.env.assertEqual(actual_result.result_set, expected)

        # empty list
        query = """UNWIND [] AS x RETURN x"""
        actual_result = redis_graph.query(query)
        expected = []
        self.env.assertEqual(actual_result.result_set, expected)

        # list with null at the last position
        query = """UNWIND [1, 2, null] AS x RETURN x"""
        actual_result = redis_graph.query(query)
        expected = [[1], [2], [None]]
        self.env.assertEqual(actual_result.result_set, expected)

        # list with null before the last position
        query = """UNWIND [1, null, 2] AS x RETURN x"""
        actual_result = redis_graph.query(query)
        expected = [[1], [None], [2]]
        self.env.assertEqual(actual_result.result_set, expected)

        # list with null at first position
        query = """UNWIND [null, 1, 2] AS x RETURN x"""
        actual_result = redis_graph.query(query)
        expected = [[None], [1], [2]]
        self.env.assertEqual(actual_result.result_set, expected)

    def test02_unwind_set(self):
        # delete property
        query = """CREATE (n:N {x:3})"""
        actual_result = redis_graph.query(query)
        query = """UNWIND ({x:null}) AS q MATCH (n:N) SET n.x= q.x RETURN n"""
        actual_result = redis_graph.query(query)
        self.env.assertEqual(actual_result.properties_removed, 1)

    def test03_pattern_comprehension(self):
        """Tests that pattern comprehensions inside an `UNWIND` clause are
        handled correctly."""

        # clean the db
        self.env.flush()
        redis_graph = Graph(self.env.getConnection(), GRAPH_ID)

        # create 2 nodes connected by an edge
        res = redis_graph.query("CREATE (:A)-[:E]->(:B)")
        self.env.assertEquals(res.nodes_created, 2)
        self.env.assertEquals(res.relationships_created, 1)

        # query with pattern comprehension in `UNWIND`
        res = redis_graph.query(
            """
            UNWIND [p = (a)-[e]->(b) | p] AS paths
            RETURN paths
            """
        )

        # assert results
        self.env.assertEquals(len(res.result_set), 1)
        n1 = Node(label="A")
        n2 = Node(label="B")
        e = Edge(0, "E", 1, edge_id=0)
        path = Path.new_empty_path().add_node(n1).add_edge(e).add_node(n2)
        self.env.assertEquals(res.result_set[0][0], path)

        # use reduce in eval expression (user failed on this)
        res = redis_graph.query(
            """
            UNWIND [p=()-[]->() | reduce(a=1,b in [1] | 1)] AS n
            RETURN n
            """
        )

        # assert results
        self.env.assertEquals(len(res.result_set), 1)
        self.env.assertEquals(res.result_set[0][0], 1)
