/*
 * Copyright Redis Ltd. 2018 - present
 * Licensed under your choice of the Redis Source Available License 2.0 (RSALv2) or
 * the Server Side Public License v1 (SSPLv1).
 */

#include "RG.h"
#include "ast.h"
#include "util.h"
#include "astnode.h"
#include "../errors.h"
#include "ast_shared.h"
#include "../util/arr.h"
#include "ast_visitor.h"
#include "../util/rax_extensions.h"
#include "../procedures/procedure.h"
#include "../arithmetic/arithmetic_expression.h"
#include <stdio.h>

typedef enum {
	NOT_DEFINED = -0x01,  // Yet to be defined
	REGULAR = 0x00,       // UNION
	ALL = 0x01            // UNION ALL
} is_union_all;

typedef struct {
	rax *defined_identifiers;      // identifiers environment
	cypher_astnode_type_t clause;  // top-level clause type
	is_union_all union_all;        // union type (regular or ALL)
	rax *intermediate_identifiers; // identifiers which are in creation in the pattern
} validations_ctx;

// ast validation visitor mappings
// number of ast-node types: _MAX_VT_OFF = sizeof(struct cypher_astnode_vts) / sizeof(struct cypher_astnode_vt *) = 114
static visit validations_mapping[114];

// validate that allShortestPaths is in a supported place
static bool _ValidateAllShortestPaths
(
	const cypher_astnode_t *root // root to validate
) {
	ASSERT(root != NULL);

	cypher_astnode_type_t t = cypher_astnode_type(root);
	// if we found allShortestPaths in invalid parent return true
	if(t == CYPHER_AST_SHORTEST_PATH &&
	   !cypher_ast_shortest_path_is_single(root)) {
		return false;
	}

	// allShortestPaths is invalid in the MATCH predicate
	if(t == CYPHER_AST_MATCH) {
		const cypher_astnode_t *predicate = cypher_ast_match_get_predicate(root);
		return predicate == NULL || _ValidateAllShortestPaths(predicate);
	}

	// recursively traverse all children
	uint nchildren = cypher_astnode_nchildren(root);
	for(uint i = 0; i < nchildren; i ++) {
		const cypher_astnode_t *child = cypher_astnode_get_child(root, i);
		if(!_ValidateAllShortestPaths(child)) {
			return false;
		}
	}

	return true;
}

// validate that shortestPaths is in a supported place
static bool _ValidateShortestPaths
(
	const cypher_astnode_t *root // root to validate
) {
	ASSERT(root != NULL);

	cypher_astnode_type_t t = cypher_astnode_type(root);
	// if we found allShortestPaths in invalid parent return true
	if(t == CYPHER_AST_SHORTEST_PATH &&
	   cypher_ast_shortest_path_is_single(root)) {
		return false;
	}

	// shortestPaths is invalid in the MATCH pattern
	if(t == CYPHER_AST_MATCH) {
		const cypher_astnode_t *pattern = cypher_ast_match_get_pattern(root);
		return _ValidateShortestPaths(pattern);
	}

	if(t == CYPHER_AST_WITH || t == CYPHER_AST_RETURN) {
		return true;
	}

	// recursively traverse all children
	uint nchildren = cypher_astnode_nchildren(root);
	for(uint i = 0; i < nchildren; i ++) {
		const cypher_astnode_t *child = cypher_astnode_get_child(root, i);
		if(!_ValidateShortestPaths(child)) {
			return false;
		}
	}

	return true;
}

// introduce aliases of a WITH clause to the bound vars
// return true if no errors where encountered, false otherwise
static bool _AST_GetWithAliases
(
	const cypher_astnode_t *node,  // ast-node from which to retrieve the aliases
	rax *aliases                   // rax to which to insert the aliases
) {
	if(!node || (cypher_astnode_type(node) != CYPHER_AST_WITH)) {
		return false;
	}

	// local env to check for duplicate column names
	rax *local_env = raxNew();

	// traverse the projections
	uint num_with_projections = cypher_ast_with_nprojections(node);
	for(uint i = 0; i < num_with_projections; i ++) {
		const cypher_astnode_t *child = cypher_ast_with_get_projection(node, i);
		const cypher_astnode_t *expr =
			cypher_ast_projection_get_expression(child);
		const cypher_astnode_t *alias_node =
			cypher_ast_projection_get_alias(child);
		void *identifier = NULL;
		const char *alias;
		uint len = 0;

		if(alias_node) {
			// retrieve "a" from "WITH [1, 2, 3] as a" or "x AS a"
			alias = cypher_ast_identifier_get_name(alias_node);
			len = strlen(alias);

			if(cypher_astnode_type(expr) == CYPHER_AST_IDENTIFIER) {
				// This is needed to validate queries where the predicate
				// include projected variables:
				// MATCH (n)-[x:T]->() WITH x AS edge WHERE (edge)-[]->()
				// load "x" from rax and store it with the alias name "edge"
				// with the same type stored previously.
				const char *current_identifier_name =
					cypher_ast_identifier_get_name(expr);
				uint len = strlen(current_identifier_name);

				// load current identifier state to project it
				identifier = raxFind(aliases,
					(unsigned char *)current_identifier_name, len);
			}

			// add the new alias to the bound vars
			raxInsert(aliases, (unsigned char *)alias, strlen(alias),
				(void *)identifier, NULL);
		} else {
			// Retrieve "a" from "WITH a"
			const cypher_astnode_t *expr = cypher_ast_projection_get_expression(child);
			if(cypher_astnode_type(expr) != CYPHER_AST_IDENTIFIER) {
				ErrorCtx_SetError("WITH clause projections must be aliased");
				raxFree(local_env);
				return false;
			}
			alias = cypher_ast_identifier_get_name(expr);
		}

		// check for duplicate column names
		if(raxTryInsert(local_env, (unsigned char *)alias, strlen(alias), NULL, NULL) == 0) {
			ErrorCtx_SetError("Error: Multiple result columns with the same name are not supported");
			raxFree(local_env);
			return false;
		}
	}

	raxFree(local_env);
	return true;
}

// Extract identifiers / aliases from a procedure call.
static void _AST_GetProcCallAliases
(
	const cypher_astnode_t *node,  // ast-node to validate
	rax *identifiers               // rax to extract identifiers to
) {
	// CALL db.labels() yield label
	// CALL db.labels() yield label as l
	ASSERT(node && identifiers);
	ASSERT(cypher_astnode_type(node) == CYPHER_AST_CALL);

	// traverse projections, collecting the identifiers and expressions
	uint projection_count = cypher_ast_call_nprojections(node);
	for(uint i = 0; i < projection_count; i++) {
		const char *identifier = NULL;
		const cypher_astnode_t *proj_node = cypher_ast_call_get_projection(node, i);
		const cypher_astnode_t *alias_node = cypher_ast_projection_get_alias(proj_node);
		if(alias_node) {
			// Alias is given: YIELD label AS l.
			identifier = cypher_ast_identifier_get_name(alias_node);
			raxInsert(identifiers, (unsigned char *)identifier, strlen(identifier), NULL, NULL);
		}
		// Introduce expression-identifier as well
		// Example: YIELD label --> label is introduced (removed outside of scope)
		const cypher_astnode_t *exp_node = cypher_ast_projection_get_expression(proj_node);
		identifier = cypher_ast_identifier_get_name(exp_node);
		raxInsert(identifiers, (unsigned char *)identifier, strlen(identifier), NULL, NULL);
	}
}

// make sure multi-hop traversals has length greater than or equal to zero
static AST_Validation _ValidateMultiHopTraversal
(
	const cypher_astnode_t *edge,  // ast-node to validate
	const cypher_astnode_t *range  // varlength range
) {
	int start = 1;
	int end = INT_MAX - 2;

	const cypher_astnode_t *range_start = cypher_ast_range_get_start(range);
	if(range_start) {
		start = AST_ParseIntegerNode(range_start);
	}

	const cypher_astnode_t *range_end = cypher_ast_range_get_end(range);
	if(range_end) {
		end = AST_ParseIntegerNode(range_end);
	}

	// Validate specified range
	if(start > end) {
		ErrorCtx_SetError("Variable length path, maximum number of hops must be greater or equal to minimum number of hops");
		return AST_INVALID;
	}

	return AST_VALID;
}

// verify that MERGE doesn't redeclare bound relations, that one reltype is
// specified for unbound relations, and that the entity is not a variable
// length pattern
static AST_Validation _ValidateMergeRelation
(
	const cypher_astnode_t *entity,  // ast-node (rel-pattern)
	rax *defined_aliases             // bound variables
) {
	// Verify that this is not a variable length relationship
	const cypher_astnode_t *range = cypher_ast_rel_pattern_get_varlength(entity);
	if(range) {
		ErrorCtx_SetError("Variable length relationships cannot be used in MERGE");
		return AST_INVALID;
	}

	const cypher_astnode_t *identifier = cypher_ast_rel_pattern_get_identifier(entity);
	const char *alias = NULL;
	if(identifier) {
		alias = cypher_ast_identifier_get_name(identifier);
		// Verify that we're not redeclaring a bound variable
		if(raxFind(defined_aliases, (unsigned char *)alias, strlen(alias)) != raxNotFound) {
			ErrorCtx_SetError("The bound variable '%s' can't be redeclared in a MERGE clause", alias);
			return AST_INVALID;
		}
	}

	// Exactly one reltype should be specified for the introduced edge
	uint reltype_count = cypher_ast_rel_pattern_nreltypes(entity);
	if(reltype_count != 1) {
		ErrorCtx_SetError("Exactly one relationship type must be specified for each relation in a MERGE pattern");
		return AST_INVALID;
	}

	// We don't need to validate the MERGE edge's direction, as an undirected edge
	// in MERGE should result a single outgoing edge to be created.

	return AST_VALID;
}

// Verify that MERGE does not introduce labels or properties to bound nodes
static AST_Validation _ValidateMergeNode
(
	const cypher_astnode_t *entity,  // ast-node
	rax *defined_aliases             // bound variables
) {
	if(raxSize(defined_aliases) == 0) {
		return AST_VALID;
	}

	const cypher_astnode_t *identifier = cypher_ast_node_pattern_get_identifier(entity);
	if(!identifier) {
		return AST_VALID;
	}

	const char *alias = cypher_ast_identifier_get_name(identifier);
	// If the entity is unaliased or not previously bound, it cannot be redeclared
	void *alias_type = raxFind(defined_aliases, (unsigned char *)alias, strlen(alias));
	if(alias_type == raxNotFound) {
		return AST_VALID;
	} else if(alias_type == (void *)T_EDGE) {
		// MATCH ()-[n]->() MERGE (n)-[:R]->()
		ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
			alias);
		return AST_INVALID;
	} else if (alias_type == (void *)T_PATH) {
		// MATCH n=() MERGE (n)-[:R]->()
		ErrorCtx_SetError("The alias '%s' was specified for both a path and a node",
			alias);
		return AST_INVALID;
	}

	// If the entity is already bound, the MERGE pattern should not introduce labels or properties
	// MATCH (n)-[:R]->() MERGE (n:L)-[:R]->()
	if(cypher_ast_node_pattern_nlabels(entity) ||
	   cypher_ast_node_pattern_get_properties(entity)) {
		ErrorCtx_SetError("The bound node '%s' can't be redeclared in a MERGE clause", alias);
		return AST_INVALID;
	}

	return AST_VALID;
}

// validate that the relation alias of an edge is not bound
static AST_Validation _ValidateCreateRelation
(
	const cypher_astnode_t *entity,  // ast-node
	rax *defined_aliases             // bounded variables
) {
	const cypher_astnode_t *identifier =
		cypher_ast_rel_pattern_get_identifier(entity);
	if(identifier) {
		const char *alias = cypher_ast_identifier_get_name(identifier);
		if(raxFind(defined_aliases, (unsigned char *)alias, strlen(alias))
				!= raxNotFound) {
			// CREATE ()-[e:R]->()-[e]->()
			ErrorCtx_SetError("The bound variable '%s' can't be redeclared in a CREATE clause", alias);
			return AST_INVALID;
		}
	}

	return AST_VALID;
}

// validate each entity referenced in a single path of a CREATE clause
static AST_Validation _Validate_CREATE_Entities
(
	const cypher_astnode_t *path,  // ast-node (pattern-path)
	rax *defined_aliases           // bound vars
) {
	uint nelems = cypher_ast_pattern_path_nelements(path);
	// redeclaration of a node is not allowed only when the path is of length 0
	// as in: MATCH (a) CREATE (a)
	// otherwise, using a defined alias of a node is allowed
	// as in: MATCH (a) CREATE (a)-[:E]->(:B)
	if(nelems == 1) {
		const cypher_astnode_t *node =
			cypher_ast_pattern_path_get_element(path, 0);
		const cypher_astnode_t *identifier =
			cypher_ast_node_pattern_get_identifier(node);
		if(identifier != NULL) {
			const char *alias = cypher_ast_identifier_get_name(identifier);
			if(raxFind(defined_aliases, (unsigned char *)alias, strlen(alias))
					!= raxNotFound) {
				ErrorCtx_SetError("The bound variable '%s' can't be redeclared in a CREATE clause", alias);
				return AST_INVALID;
			}
		}
	}
	return AST_VALID;
}

// validate each entity referenced in a single path of a MATCH clause
static AST_Validation _Validate_MATCH_Entities
(
	const cypher_astnode_t *path,  // ast-node (pattern-path)
	rax *defined_aliases           // bound vars
) {
	uint nelems = cypher_ast_pattern_path_nelements(path);

	rax *local_env = raxNew();

	for(uint i = 0 ; i < nelems; i++) {
		const cypher_astnode_t *path_elem = cypher_ast_pattern_path_get_element(path, i);
		const cypher_astnode_t *current_identifier = NULL;

		if(i % 2 == 0) {
			current_identifier = cypher_ast_node_pattern_get_identifier(path_elem);
		} else {
			current_identifier = cypher_ast_rel_pattern_get_identifier(path_elem);
		}

		if(current_identifier) {
			const char *identifier_name =
				cypher_ast_identifier_get_name(current_identifier);
			int len = strlen(identifier_name);
			void *identifier = raxFind(defined_aliases,
							  (unsigned char *)identifier_name, len);

			if(i % 2 == 0) {
				// validate nodes

				if(identifier == (void *)T_EDGE) {
					// MATCH ()-[r]->() MATCH(r)
					ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
						identifier_name);
					break;
				} else if (identifier == (void *)T_PATH) {
					// MATCH n=() MATCH (n)-[:R]->()
					ErrorCtx_SetError("The alias '%s' was specified for both a path and a node",
						identifier_name);
					break;
				}

				// MATCH (r)-[r]-() RETURN 0
				void *node_identifier = raxFind(local_env,
									(unsigned char *)identifier_name, len);

				if(node_identifier == raxNotFound) {
					raxInsert(local_env, (unsigned char *) identifier_name,
						len, (void *)T_NODE, NULL);
				} else if(node_identifier != (void *)T_NODE) {
					ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
						identifier_name);
					break;
				}
			} else {
				// validate edges

				// if the identifier was defined previously, out of this
				// pattern, its type must be T_EDGE or NULL.
				// These queries are valid:
				// MATCH (a) WITH a MATCH (a:L)-[e]->(b) RETURN a
				// WITH NULL AS e MATCH (a:L)-[e]->(b) RETURN e
				// but these are not valid:
				// MATCH (n) WITH n AS e MATCH (a:L)-[e]->(b) RETURN e
				// TODO: This query is being accepted, but it is invalid:
				// WITH 1 AS x MATCH ()-[x]->() RETURN 0
				if(identifier == (void *)T_NODE) {
					ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
						identifier_name);
					break;
				} else if (identifier == (void *)T_PATH) {
					ErrorCtx_SetError("The alias '%s' was specified for both a path and a relationship",
						identifier_name);
					break;
				}

				// validation using local env, because until now, these
				// identifiers are not part of the clause context
				void *edge_identifier = raxFind(local_env,
									(unsigned char *)identifier_name, len);

				if(edge_identifier == raxNotFound) {
					raxInsert(local_env, (unsigned char *)identifier_name,
						len, (void *)T_EDGE, NULL);
				} else if(edge_identifier == (void *)T_EDGE) {
					// the edge label must be unique inside the pattern path
					// MATCH (a)-[r]->()-[r]->(a) RETURN 0
					ErrorCtx_SetError("Cannot use the same relationship variable '%s'",
						identifier_name);
					break;
				} else {
					ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
						identifier_name);
					break;

				}
			}
		}
	}

	raxFree(local_env);
	if(ErrorCtx_EncounteredError()) {
		return AST_INVALID;
	}
	return AST_VALID;
}

// make sure an identifier is bound
static AST_Validation _Validate_referred_identifier
(
	const char *identifier, // identifier to check
	ast_visitor *visitor    // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	int len = strlen(identifier);

	// emit error if the identifier is not defined
	// MATCH (n) DELETE x
	if(raxFind(vctx->defined_identifiers, (unsigned char *)identifier, len) == raxNotFound) {
		ErrorCtx_SetError("'%.*s' not defined", len, identifier);
		return AST_INVALID;
	}

	// emit an error if the identifier is an intermediate entity
	// CREATE (a {v:0}), ()-[:R {k:toJSON(a)}]->()
	void *intermediate =
		raxFind(vctx->intermediate_identifiers, (unsigned char *)identifier, len);
	if(intermediate != raxNotFound && intermediate != NULL) {
		ErrorCtx_SetError("'%.*s' not defined", len, identifier);
		return AST_INVALID;
	}

	return AST_VALID;
}

// validate a list comprehension
static VISITOR_STRATEGY _Validate_list_comprehension
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	// we enter ONLY when start=true, so no check is needed

	// list comprehension example:
	// RETURN [x IN range(1,5) WHERE x % 3 = 0 | x^2 ] AS r
	// the identifier is: x
	// the expression is: range(1,5)
	// the predicate is:  x % 3 = 0
	// the eval is:       x^2

	// build a new environment of bounded vars from the current one to be
	// used in the traversal of the visitor in the pattern comprehension
	// validation, because they are local to the pattern
	rax *orig_env = vctx->defined_identifiers;
	rax *scoped_env = raxClone(orig_env);
	vctx->defined_identifiers = scoped_env;

	const cypher_astnode_t *id =
		cypher_ast_list_comprehension_get_identifier(n);
	const char *identifier = cypher_ast_identifier_get_name(id);

	// Introduce local identifier if it is not yet
	// if the identifier exists, it will be overwritten in scoped environment
	raxInsert(vctx->defined_identifiers, (unsigned char *)identifier,
		strlen(identifier), NULL, NULL);

	// Visit expression-children
	// Visit expression
	const cypher_astnode_t *exp = cypher_ast_list_comprehension_get_expression(n);
	if(exp) {
		// RETURN [x IN range(1,u) WHERE x > 3 | x ]
		AST_Visitor_visit(exp, visitor);
		if(ErrorCtx_EncounteredError()) {
			goto cleanup;
		}
	}

	// Visit predicate
	const cypher_astnode_t *pred = cypher_ast_list_comprehension_get_predicate(n);
	if(pred) {
		// RETURN [x IN range(1,10) WHERE u > 3 | x ]
		AST_Visitor_visit(pred, visitor);
		if(ErrorCtx_EncounteredError()) {
			goto cleanup;
		}
	}

	// Visit eval
	const cypher_astnode_t *eval = cypher_ast_list_comprehension_get_eval(n);
	if(eval) {
		// RETURN [x IN range(1,10) WHERE x > 3 | u ]
		AST_Visitor_visit(eval, visitor);
		if(ErrorCtx_EncounteredError()) {
			goto cleanup;
		}
	}

cleanup:
	// restore original environment of bounded vars
	vctx->defined_identifiers = orig_env;
	raxFree(scoped_env);

	// check for errors
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate a pattern comprehension
static VISITOR_STRATEGY _Validate_pattern_comprehension
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	// we enter ONLY when start=true, so no check is needed

	// pattern comprehension example:
	// MATCH (a) RETURN [p = (a)-[e]->(f) WHERE e.v < 10 | f]
	// the identifier is: p
	// the pattern is:    (a)-[e]->(f)
	// the predicate is:  e.v < 10
	// the eval is:       f
	const char *identifier;

	// build a new environment of bounded vars from the current one to be
	// used in the traversal of the visitor in the pattern comprehension
	// validation, because they are local to the pattern
	rax *orig_env = vctx->defined_identifiers;
	rax *scoped_env = raxClone(orig_env);
	vctx->defined_identifiers = scoped_env;

	const cypher_astnode_t *id =
		cypher_ast_pattern_comprehension_get_identifier(n);

	if(id) {
		identifier = cypher_ast_identifier_get_name(id);

		// introduce local identifier if it is not yet introduced
		// if the identifier exists, it will be overwritten in scoped environment
		// MATCH (a) RETURN [a=()-[]->() | 0]
		raxInsert(vctx->defined_identifiers, (unsigned char *)identifier,
			strlen(identifier), (void *)T_PATH, NULL);
	}

	// visit expression-children
	// visit pattern
	const cypher_astnode_t *pattern =
		cypher_ast_pattern_comprehension_get_pattern(n);
	if(pattern != NULL) {
		cypher_astnode_type_t node_type = cypher_astnode_type(pattern);
		// the pattern might include new identifiers, then we set
		// the clause temporarly to CYPHER_AST_PATTERN_COMPREHENSION
		// to prevent the validation of referred identifiers
		cypher_astnode_type_t clause_backup = vctx->clause;
		vctx->clause = CYPHER_AST_PATTERN_COMPREHENSION;
		AST_Visitor_visit(pattern, visitor);
		vctx->clause = clause_backup;
		if(ErrorCtx_EncounteredError()) {
			// MATCH (a) RETURN [(b)-[b]->() | 0]
			goto cleanup;
		}
	}

	// visit predicate
	const cypher_astnode_t *pred =
		cypher_ast_pattern_comprehension_get_predicate(n);
	if(pred) {
		AST_Visitor_visit(pred, visitor);
		if(ErrorCtx_EncounteredError()) {
			// MATCH (a) RETURN [(a)-[:R]->() WHERE u | 0]
			goto cleanup;
		}
	}

	// visit eval
	const cypher_astnode_t *eval = cypher_ast_pattern_comprehension_get_eval(n);
	if(eval) {
		AST_Visitor_visit(eval, visitor);
		if(ErrorCtx_EncounteredError()) {
			// MATCH (a) RETURN [(a)-[:R]->() | u]
			goto cleanup;
		}
	}

cleanup:
	// restore original environment of bounded vars
	vctx->defined_identifiers = orig_env;
	raxFree(scoped_env);

	// check for errors
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate that an identifier is bound
static VISITOR_STRATEGY _Validate_identifier
(
	const cypher_astnode_t *n,  // ast-node (identifier)
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	const char *identifier = cypher_ast_identifier_get_name(n);

	if(_Validate_referred_identifier(identifier, visitor) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	return VISITOR_RECURSE;
}

// validate the values of a map
static VISITOR_STRATEGY _Validate_map
(
	const cypher_astnode_t *n,  // ast-node (map)
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	// we enter ONLY when start=true, so no check is needed

	// traverse the entries of the map
	uint nentries = cypher_ast_map_nentries(n);
	for (uint i = 0; i < nentries; i++) {
		const cypher_astnode_t *exp = cypher_ast_map_get_value(n, i);
		AST_Visitor_visit(exp, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate a projection
static VISITOR_STRATEGY _Validate_projection
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	// we enter ONLY when start=true, so no check is needed

	const cypher_astnode_t *exp = cypher_ast_projection_get_expression(n);
	AST_Visitor_visit(exp, visitor);
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate a function-call
static AST_Validation _ValidateFunctionCall
(
	const char *funcName,    // function name
	bool include_aggregates  // are aggregations allowed
) {
	// check existence of the function-name
	if(!AR_FuncExists(funcName)) {
		ErrorCtx_SetError("Unknown function '%s'", funcName);
		return AST_INVALID;
	}

	if(!include_aggregates && AR_FuncIsAggregate(funcName)) {
		// Provide a unique error for using aggregate functions from inappropriate contexts
		ErrorCtx_SetError("Invalid use of aggregating function '%s'", funcName);
		return AST_INVALID;
	}

	return AST_VALID;
}

// validate an apply-all operator
static VISITOR_STRATEGY _Validate_apply_all_operator
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	if(!start) {
		return VISITOR_CONTINUE;
	}

	// Working with a function call that has * as its argument.
	const cypher_astnode_t *func = cypher_ast_apply_all_operator_get_func_name(n);
	const char *func_name = cypher_ast_function_name_get_value(func);

	// Verify that this is a COUNT call.
	if(strcasecmp(func_name, "COUNT")) {
		ErrorCtx_SetError("COUNT is the only function which can accept * as an argument");
		return VISITOR_BREAK;
	}

	// Verify that DISTINCT is not specified.
	if(cypher_ast_apply_all_operator_get_distinct(n)) {
		// TODO consider opening a parser error, this construction is invalid in Neo's parser.
		ErrorCtx_SetError("Cannot specify both DISTINCT and * in COUNT(DISTINCT *)");
		return VISITOR_BREAK;
	}

	return VISITOR_RECURSE;
}

// validate an apply operator
static VISITOR_STRATEGY _Validate_apply_operator
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	if(!start) {
		return VISITOR_CONTINUE;
	}

	// Collect the function name.
	const cypher_astnode_t *func = cypher_ast_apply_operator_get_func_name(n);
	const char *func_name = cypher_ast_function_name_get_value(func);
	// MATCH (m:P)--(y:P) WITH m.age AS age, count(y.age) AS cnt ORDER BY m.age + count(y.age) RETURN age
	if(_ValidateFunctionCall(func_name, (vctx->clause == CYPHER_AST_WITH ||
										vctx->clause == CYPHER_AST_APPLY_OPERATOR ||
										vctx->clause == CYPHER_AST_RETURN)) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	return VISITOR_RECURSE;
}

// validate reduce
static VISITOR_STRATEGY _Validate_reduce
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	if(!start) {
		return VISITOR_CONTINUE;
	}

	cypher_astnode_type_t orig_clause = vctx->clause;
	// change clause type of vctx so that function-validation will work properly
	// (include-aggregations should be set to false)
	vctx->clause = CYPHER_AST_REDUCE;

	// A reduce call has an accumulator and a local list variable that should
	// only be accessed within its scope;
	// do not leave them in the identifiers map
	// example: reduce(sum=0, n in [1,2] | sum+n)
	//  the reduce function is composed of 5 components:
	//     1. accumulator                  `sum`
	//     2. accumulator init expression  `0`
	//     3. list expression              `[1,2,3]`
	//     4. variable                     `n`
	//     5. eval expression              `sum + n`

	// make sure that the init expression is a known var or valid exp.
	const cypher_astnode_t *init_node = cypher_ast_reduce_get_init(n);
	if(cypher_astnode_type(init_node) == CYPHER_AST_IDENTIFIER) {
		// check if the variable has already been introduced
		const char *var_str = cypher_ast_identifier_get_name(init_node);
		if(raxFind(vctx->defined_identifiers, (unsigned char *)var_str, strlen(var_str)) == raxNotFound) {
			ErrorCtx_SetError("'%s' not defined", var_str);
			return VISITOR_BREAK;
		}
	}
	else {
		AST_Visitor_visit(init_node, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// make sure that the list expression is a list (or list comprehension) or an
	// alias of an existing one.
	const cypher_astnode_t *list_var = cypher_ast_reduce_get_expression(n);
	if(cypher_astnode_type(list_var) == CYPHER_AST_IDENTIFIER) {
		const char *list_var_str = cypher_ast_identifier_get_name(list_var);
		if(raxFind(vctx->defined_identifiers, (unsigned char *) list_var_str, strlen(list_var_str)) == raxNotFound) {
			ErrorCtx_SetError("'%s' not defined", list_var_str);
			return VISITOR_BREAK;
		}
	}

	// Visit the list expression (no need to introduce local vars)
	AST_Visitor_visit(list_var, visitor);
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// make sure that the eval-expression exists
	const cypher_astnode_t *eval_node = cypher_ast_reduce_get_eval(n);
	if(!eval_node) {
		ErrorCtx_SetError("No eval expression given in reduce");
		return VISITOR_BREAK;
	}

	// If accumulator is already in the environment, don't reintroduce it
	const cypher_astnode_t *accum_node =cypher_ast_reduce_get_accumulator(n);
	const char *accum_str = cypher_ast_identifier_get_name(accum_node);
	bool introduce_accum =
	(raxFind(vctx->defined_identifiers, (unsigned char *) accum_str, strlen(accum_str))
			 == raxNotFound);
	if(introduce_accum)
		raxInsert(vctx->defined_identifiers, (unsigned char *) accum_str, strlen(accum_str), NULL, NULL);

	// same for the list var
	const cypher_astnode_t *list_var_node =cypher_ast_reduce_get_identifier(n);
	const char *list_var_str = cypher_ast_identifier_get_name(list_var_node);
	bool introduce_list_var = (raxFind(vctx->defined_identifiers, (unsigned char *) list_var_str, strlen(list_var_str))
						    == raxNotFound);
	if(introduce_list_var)
		raxInsert(vctx->defined_identifiers, (unsigned char *) list_var_str, strlen(list_var_str), NULL, NULL);

	// visit eval expression
	const cypher_astnode_t *eval_exp = cypher_ast_reduce_get_eval(n);
	AST_Visitor_visit(eval_exp, visitor);
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// change clause type back
	vctx->clause = orig_clause;

	// Remove local vars\aliases if introduced
	if(introduce_accum) {
		raxRemove(vctx->defined_identifiers, (unsigned char *) accum_str, strlen(accum_str), NULL);
	}
	if(introduce_list_var) {
		raxRemove(vctx->defined_identifiers, (unsigned char *) list_var_str, strlen(list_var_str), NULL);
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate the property maps used in node/edge patterns
// in MATCH and CREATE clauses
static AST_Validation _ValidateInlinedProperties
(
	const cypher_astnode_t *props, // ast-node representing the map
	const char *alias,             // alias of current node/edge
	ast_visitor *visitor		   // visitor
) {
	if(props == NULL) {
		return AST_VALID;
	}
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	// emit an error if the properties are not presented as a map, as in:
	// MATCH (p {invalid_property_construction}) RETURN p
	if(cypher_astnode_type(props) != CYPHER_AST_MAP) {
		ErrorCtx_SetError("Encountered unhandled type in inlined properties");
		return AST_INVALID;
	}

	// traverse map entries
	uint prop_count = cypher_ast_map_nentries(props);
	for(uint i = 0; i < prop_count; i++) {
		const cypher_astnode_t *prop_val = cypher_ast_map_get_value(props, i);

		//----------------------------------------------------------------------
		// validate each property value
		//----------------------------------------------------------------------

		cypher_astnode_type_t type = cypher_astnode_type(prop_val);

		if (type == CYPHER_AST_PATTERN_PATH) {
			// encountered query of the form:
			// MATCH (a {prop: ()-[]->()}) RETURN a
			ErrorCtx_SetError("Encountered unhandled type in inlined properties");
			return AST_INVALID;
		} else if(type == CYPHER_AST_IDENTIFIER) {
			const char *identifier_name = cypher_ast_identifier_get_name(prop_val);
			uint len = strlen(identifier_name);

			// emit an error if the property is an undefined variable
			// CREATE (a {name: missing})
			// CREATE (a {v:b}), (b)
			void *identifier_type = raxFind(vctx->defined_identifiers,
				(unsigned char *)identifier_name, len);

			if(identifier_type == raxNotFound) {
				ErrorCtx_SetError("'%.*s' not defined", len, identifier_name);
				return AST_INVALID;
			}

			// detect undefined variables used in the same pattern
			// CREATE (a:A)-[:R]->(b:B {v:a})
			void *intermediate = raxFind(vctx->intermediate_identifiers,
				(unsigned char *)identifier_name, len);

			if(intermediate != raxNotFound && intermediate != NULL) {
				ErrorCtx_SetError("'%.*s' not defined", len, identifier_name);
				return AST_INVALID;
			}

			// emit an error if the property value is of type node or edge
			// CREATE (a:A) WITH a CREATE (b:B {v:a})
			if(identifier_type == (void *)T_NODE ||
			   identifier_type == (void *)T_EDGE) {
				ErrorCtx_SetError("Property values can only be of primitive types or arrays of primitive types");
				return AST_INVALID;
			}
		} else if(type == CYPHER_AST_PROPERTY_OPERATOR) {
			const cypher_astnode_t *exp =
				cypher_ast_property_operator_get_expression(prop_val);
			if(cypher_astnode_type(exp) == CYPHER_AST_IDENTIFIER) {
				const char *identifier_name = cypher_ast_identifier_get_name(exp);
				uint len = strlen(identifier_name);

				// emit an error if the property reference to a property of
				// the same node:
				// CREATE (a {v:a.p})
				// CREATE ()-[r {v:r.x}]->()
				if(vctx->clause != CYPHER_AST_MATCH && alias != NULL &&
					strcmp(alias, identifier_name) == 0) {
					ErrorCtx_SetError("'%.*s' not defined", len, identifier_name);
					return AST_INVALID;
				}

				// emit an error if the property references to an "intermediate"
				// node or edge:
				// CREATE (a:A), (b:B {v:a.v})
				void *identifier_type = raxFind(vctx->intermediate_identifiers,
					(unsigned char *)identifier_name, len);

				if(identifier_type == (void *)T_NODE ||
				   identifier_type == (void *)T_EDGE) {
					ErrorCtx_SetError("'%.*s' not defined", len, identifier_name);
					return AST_INVALID;
				}
			}
		} else if (type == CYPHER_AST_SUBSCRIPT_OPERATOR) {
			const cypher_astnode_t *exp =
				cypher_ast_subscript_operator_get_expression(prop_val);
			if(cypher_astnode_type(exp) == CYPHER_AST_IDENTIFIER) {
				const char *identifier_name = cypher_ast_identifier_get_name(exp);
				uint len = strlen(identifier_name);

				void *identifier_type = raxFind(vctx->defined_identifiers,
					   		(unsigned char *)identifier_name, len);

				// emit an error if the property is an undefined variable
				// CREATE ({v:bar[0]})
				if(identifier_type == raxNotFound) {
					ErrorCtx_SetError("'%.*s' not defined", len, identifier_name);
					return AST_INVALID;
				}
			}
		} else {
			// visit property value
			AST_Visitor_visit(prop_val, visitor);
			if(ErrorCtx_EncounteredError()) {
				return VISITOR_BREAK;
			}
		}
	}

	return AST_VALID;
}

// validate a pattern
static VISITOR_STRATEGY _Validate_pattern
(
	const cypher_astnode_t *n,  // ast-node (rel-pattern)s
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		// free intermediate_identifiers because the validation of the
		// current pattern has finished
		raxFree(vctx->intermediate_identifiers);
		vctx->intermediate_identifiers = raxNew();
		return VISITOR_CONTINUE;
	}

	return VISITOR_RECURSE;
}

// validate a relation-pattern
static VISITOR_STRATEGY _Validate_rel_pattern
(
	const cypher_astnode_t *n,  // ast-node (rel-pattern)s
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	const cypher_astnode_t *alias_edge =
		cypher_ast_rel_pattern_get_identifier(n);
	const char *alias = NULL;

	if(alias_edge) {
		alias = cypher_ast_identifier_get_name(alias_edge);
	}

	const cypher_astnode_t *range = cypher_ast_rel_pattern_get_varlength(n);
	if(vctx->clause == CYPHER_AST_CREATE) {
		// validate that the relation alias is not bound
		if(_ValidateCreateRelation(n, vctx->defined_identifiers) == AST_INVALID) {
			return VISITOR_BREAK;
		}

		// validate that each relation has exactly one type
		uint reltype_count = cypher_ast_rel_pattern_nreltypes(n);
		if(reltype_count != 1) {
			// CREATE ()-[e]->()
			ErrorCtx_SetError("Exactly one relationship type must be specified for CREATE");
			return VISITOR_BREAK;
		}

		// validate that each relation being created is directed
		if(cypher_ast_rel_pattern_get_direction(n) == CYPHER_REL_BIDIRECTIONAL) {
			ErrorCtx_SetError("Only directed relationships are supported in CREATE");
			return VISITOR_BREAK;
		}

		// validate that each relation being created is not variable length relationship
		if(range != NULL) {
			ErrorCtx_SetError("Variable length relationships cannot be used in CREATE");
			return VISITOR_BREAK;
		}
	}

	if(_ValidateInlinedProperties(cypher_ast_rel_pattern_get_properties(n),
			alias, visitor) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	if(vctx->clause == CYPHER_AST_MERGE &&
		_ValidateMergeRelation(n, vctx->defined_identifiers) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	// if this is a multi-hop traversal, validate it accordingly
	if(range && _ValidateMultiHopTraversal(n, range) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	if(alias != NULL) {
		void *alias_type = raxFind(vctx->defined_identifiers,
				(unsigned char *)alias, strlen(alias));

		if(alias_type != raxNotFound) {
			// we can't register an edge more than once
			// following are all illegal examples
			// MATCH (a) RETURN [(b)-[b]->() | 0]"
			// CREATE ()-[e]->()-[e]->()
			// MATCH (a) RETURN [()-[b]->()-[b]->() | 0]
			// CYPHER_AST_MATCH was validated by _Validate_MATCH_Entities()
			if(vctx->clause != CYPHER_AST_MATCH) {
				if(alias_type == (void *)T_EDGE) {
					ErrorCtx_SetError("Cannot use the same relationship variable '%s'",
						alias);
				} else if(alias_type == (void *)T_NODE) {
					ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
						alias);
				} else if(alias_type == (void *)T_PATH) {
					ErrorCtx_SetError("The alias '%s' was specified for both a path and a relationship",
						alias);
				}

				return VISITOR_BREAK;
			}
		} else {
			// the edge is valid
			// register edge under its alias
			raxInsert(vctx->defined_identifiers, (unsigned char *)alias,
					strlen(alias), (void *)T_EDGE, NULL);

			// if it is a CREATE clause register edge as an intermediate
			// identifier because it is under creation
			if(vctx->clause == CYPHER_AST_CREATE) {
				raxInsert(vctx->intermediate_identifiers,
					(unsigned char *)alias, strlen(alias),
					(void *)T_EDGE, NULL);
			}
		}
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate a node-pattern expression
static VISITOR_STRATEGY _Validate_node_pattern
(
	const cypher_astnode_t *n,  // ast-node (node-pattern)
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	const cypher_astnode_t *alias_node =
		cypher_ast_node_pattern_get_identifier(n);
	const char *alias = NULL;

	if(alias_node != NULL) {
		alias = cypher_ast_identifier_get_name(alias_node);
	}

	if(_ValidateInlinedProperties(cypher_ast_node_pattern_get_properties(n),
			alias, visitor) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	if(vctx->clause == CYPHER_AST_MERGE) {
		if(_ValidateMergeNode(n, vctx->defined_identifiers) == AST_INVALID) {
			return VISITOR_BREAK;
		}
	}

	if(alias != NULL) {

		void *alias_type =
			raxFind(vctx->defined_identifiers, (unsigned char *)alias,
					strlen(alias));

		if(alias_type == (void *)T_EDGE) {
			// MATCH ()-[n]->() CREATE (n)-[:R]->()
			ErrorCtx_SetError("The alias '%s' was specified for both a node and a relationship",
				alias);
			return VISITOR_BREAK;
		} else if (alias_type == (void *)T_PATH) {
			ErrorCtx_SetError("The alias '%s' was specified for both a path and a node",
				alias);
			return VISITOR_BREAK;
		}

		// the sub tree spanned from this node is valid
		raxInsert(vctx->defined_identifiers, (unsigned char *)alias,
			strlen(alias), (void *)T_NODE, NULL);

		if(alias_type == raxNotFound) {
			// if it is a CREATE clause register edge as an intermediate
			// identifier because it is under creation
			if(vctx->clause == CYPHER_AST_CREATE) {
				raxInsert(vctx->intermediate_identifiers,
					(unsigned char *)alias, strlen(alias),
					(void *)T_NODE, NULL);
			}
		}
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate a shortest-path expression
static VISITOR_STRATEGY _Validate_shortest_path
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	if(!start) {
		return VISITOR_CONTINUE;
	}

	if(cypher_ast_shortest_path_is_single(n)) {
		const cypher_astnode_t *path = cypher_ast_shortest_path_get_path(n);
		int elements = cypher_ast_pattern_path_nelements(path);
		const cypher_astnode_t *start = cypher_ast_node_pattern_get_identifier(cypher_ast_pattern_path_get_element(path, 0));
		const cypher_astnode_t *end = cypher_ast_node_pattern_get_identifier(cypher_ast_pattern_path_get_element(path, elements - 1));
		if(start == NULL || end == NULL) {
			ErrorCtx_SetError("A shortestPath requires bound nodes");
			return VISITOR_BREAK;
		}
		const char *start_id = cypher_ast_identifier_get_name(start);
		const char *end_id = cypher_ast_identifier_get_name(end);
		if(raxFind(vctx->defined_identifiers, (unsigned char *)start_id, strlen(start_id)) == raxNotFound ||
			raxFind(vctx->defined_identifiers, (unsigned char *)end_id, strlen(end_id)) == raxNotFound) {
			ErrorCtx_SetError("A shortestPath requires bound nodes");
			return VISITOR_BREAK;
		}
		return VISITOR_RECURSE;
	} else {
		// MATCH (a), (b), p = allShortestPaths((a)-[*2..]->(b)) RETURN p
		// validate rel pattern range doesn't contains a minimum > 1
		const cypher_astnode_t **ranges = AST_GetTypedNodes(n, CYPHER_AST_RANGE);
		int range_count = array_len(ranges);
		for(int i = 0; i < range_count; i++) {
			long min_hops = 1;
			const cypher_astnode_t *r = ranges[i];
			const cypher_astnode_t *start = cypher_ast_range_get_start(r);
			if(start) min_hops = AST_ParseIntegerNode(start);
			if(min_hops != 1) {
				ErrorCtx_SetError("allShortestPaths(...) does not support a minimal length different from 1");
				break;
			}
		}
		array_free(ranges);
	}

	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	return VISITOR_RECURSE;
}

// validate a pattern-path expression
static VISITOR_STRATEGY _Validate_pattern_path
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	if(vctx->clause == CYPHER_AST_CREATE &&
		_Validate_CREATE_Entities(n, vctx->defined_identifiers) == AST_INVALID) {
		return VISITOR_BREAK;
	} else if(vctx->clause == CYPHER_AST_MATCH &&
		_Validate_MATCH_Entities(n, vctx->defined_identifiers) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	return VISITOR_RECURSE;
}

// validate a named path
static VISITOR_STRATEGY _Validate_named_path
(
	const cypher_astnode_t *n,  // ast-node (named path)
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	// introduce identifiers to bound variables environment
	const cypher_astnode_t *alias_node = cypher_ast_named_path_get_identifier(n);
	const char *alias = cypher_ast_identifier_get_name(alias_node);
	uint len = strlen(alias);

	void *alias_type =
		raxFind(vctx->defined_identifiers, (unsigned char *)alias, len);

	if(alias_type == (void *)T_EDGE) {
		// MATCH m=() WITH m AS n MATCH ()-[n:R]->()
		ErrorCtx_SetError("The alias '%s' was specified for both a path and a relationship",
			alias);
		return VISITOR_BREAK;
	} else if (alias_type == (void *)T_NODE) {
		// MATCH m=() WITH m AS n MATCH (n)-[:R]->()
		ErrorCtx_SetError("The alias '%s' was specified for both a path and a node",
			alias);
		return VISITOR_BREAK;
	}

	raxInsert(vctx->defined_identifiers, (unsigned char *)alias, len,
		   (void *)T_PATH, NULL);

	return VISITOR_RECURSE;
}

// validate limit and skip modifiers
static AST_Validation _Validate_LIMIT_SKIP_Modifiers
(
	const cypher_astnode_t *limit,  // limit ast-node
	const cypher_astnode_t *skip    // skip ast-node
) {
	if(limit) {
		// Handle non-integer or non parameter types specified as LIMIT value
		// The value validation of integer node or parameter node is done in run time evaluation.
		if(cypher_astnode_type(limit) != CYPHER_AST_INTEGER &&
			cypher_astnode_type(limit) != CYPHER_AST_PARAMETER) {
			ErrorCtx_SetError("LIMIT specified value of invalid type, must be a positive integer");
			return AST_INVALID;
		}
	}

	if(skip) {
		// Handle non-integer or non parameter types specified as skip value
		// The value validation of integer node or parameter node is done in run time evaluation.
		if(cypher_astnode_type(skip) != CYPHER_AST_INTEGER &&
			cypher_astnode_type(skip) != CYPHER_AST_PARAMETER) {
			ErrorCtx_SetError("SKIP specified value of invalid type, must be a positive integer");
			return AST_INVALID;
		}
	}

	return AST_VALID;
}

// validate UNION clauses
static AST_Validation _ValidateUnion_Clauses
(
	const AST *ast  // ast-node
) {
	// get amount of UNION clauses
	AST_Validation res = AST_VALID;
	uint *union_indices = AST_GetClauseIndices(ast, CYPHER_AST_UNION);
	uint union_clause_count = array_len(union_indices);
	array_free(union_indices);

	if(union_clause_count == 0) {
		return AST_VALID;
	}

	// Require all RETURN clauses to perform the exact same projection
	uint *return_indices = AST_GetClauseIndices(ast, CYPHER_AST_RETURN);
	uint return_clause_count = array_len(return_indices);

	// We should have one more RETURN clauses than we have UNION clauses.
	if(return_clause_count != union_clause_count + 1) {
		ErrorCtx_SetError("Found %d UNION clauses but only %d RETURN clauses", union_clause_count,
						  return_clause_count);
		array_free(return_indices);
		return AST_INVALID;
	}

	const cypher_astnode_t *return_clause = cypher_ast_query_get_clause(ast->root, return_indices[0]);
	uint proj_count = cypher_ast_return_nprojections(return_clause);
	const char *projections[proj_count];

	for(uint j = 0; j < proj_count; j++) {
		const cypher_astnode_t *proj = cypher_ast_return_get_projection(return_clause, j);
		const cypher_astnode_t *alias_node = cypher_ast_projection_get_alias(proj);
		if(alias_node == NULL)  {
			// The projection was not aliased, so the projection itself must be an identifier.
			alias_node = cypher_ast_projection_get_expression(proj);
			ASSERT(cypher_astnode_type(alias_node) == CYPHER_AST_IDENTIFIER);
		}
		const char *alias = cypher_ast_identifier_get_name(alias_node);
		projections[j] = alias;
	}

	for(uint i = 1; i < return_clause_count; i++) {
		return_clause = cypher_ast_query_get_clause(ast->root, return_indices[i]);
		if(proj_count != cypher_ast_return_nprojections(return_clause)) {
			ErrorCtx_SetError("All sub queries in a UNION must have the same column names");
			res = AST_INVALID;
			goto cleanup;
		}

		for(uint j = 0; j < proj_count; j++) {
			const cypher_astnode_t *proj = cypher_ast_return_get_projection(return_clause, j);
			const cypher_astnode_t *alias_node = cypher_ast_projection_get_alias(proj);
			if(alias_node == NULL)  {
				// The projection was not aliased, so the projection itself must be an identifier.
				alias_node = cypher_ast_projection_get_expression(proj);
				ASSERT(cypher_astnode_type(alias_node) == CYPHER_AST_IDENTIFIER);
			}
			const char *alias = cypher_ast_identifier_get_name(alias_node);
			if(strcmp(projections[j], alias) != 0) {
				ErrorCtx_SetError("All sub queries in a UNION must have the same column names");
				res = AST_INVALID;
				goto cleanup;
			}
		}
	}

cleanup:
	array_free(return_indices);
	return res;
}

// validate a CALL clause
static VISITOR_STRATEGY _Validate_CALL_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(start) {
		vctx->clause = cypher_astnode_type(n);
		// introduce aliases in the clause to the bounded vars environment
		_AST_GetProcCallAliases(n, vctx->defined_identifiers);

		/* Make sure procedure calls are valid:
		* 1. procedure exists
		* 2. number of arguments to procedure is as expected
		* 3. yield refers to procedure output */

		ProcedureCtx *proc = NULL;
		rax *rax = NULL;

		// Make sure procedure exists.
		const char *proc_name = cypher_ast_proc_name_get_value(cypher_ast_call_get_proc_name(n));
		proc = Proc_Get(proc_name);

		if(proc == NULL) {
			ErrorCtx_SetError("Procedure `%s` is not registered", proc_name);
			goto cleanup;
		}

		// Validate num of arguments.
		if(proc->argc != PROCEDURE_VARIABLE_ARG_COUNT) {
			unsigned int given_arg_count = cypher_ast_call_narguments(n);
			if(Procedure_Argc(proc) != given_arg_count) {
				ErrorCtx_SetError("Procedure `%s` requires %d arguments, got %d", proc_name, proc->argc,
									given_arg_count);
				goto cleanup;
			}
		}

		rax = raxNew();

		// validate projections
		uint proj_count = cypher_ast_call_nprojections(n);
		// collect call projections
		for(uint j = 0; j < proj_count; j++) {
			const cypher_astnode_t *proj = cypher_ast_call_get_projection(n, j);
			const cypher_astnode_t *ast_exp = cypher_ast_projection_get_expression(proj);
			ASSERT(cypher_astnode_type(ast_exp) == CYPHER_AST_IDENTIFIER);
			const char *identifier = cypher_ast_identifier_get_name(ast_exp);

			// make sure each yield output is mentioned only once
			if(!raxInsert(rax, (unsigned char *)identifier, strlen(identifier), NULL, NULL)) {
				ErrorCtx_SetError("Variable `%s` already declared", identifier);
				goto cleanup;
			}

			// make sure procedure is aware of output
			if(!Procedure_ContainsOutput(proc, identifier)) {
				ErrorCtx_SetError("Procedure `%s` does not yield output `%s`",
									proc_name, identifier);
				goto cleanup;
			}
		}

cleanup:
		if(proc) {
			Proc_Free(proc);
		}
		if(rax) {
			raxFree(rax);
		}
		return !ErrorCtx_EncounteredError() ? VISITOR_RECURSE : VISITOR_BREAK;
	}

	// end handling

	uint proj_count = cypher_ast_call_nprojections(n);
	// remove expression identifiers from bound vars if an alias exists
	for(uint j = 0; j < proj_count; j++) {
		const cypher_astnode_t *proj = cypher_ast_call_get_projection(n, j);
		const cypher_astnode_t *ast_exp = cypher_ast_projection_get_expression(proj);
		ASSERT(cypher_astnode_type(ast_exp) == CYPHER_AST_IDENTIFIER);
		const char *identifier = cypher_ast_identifier_get_name(ast_exp);
		if(cypher_ast_projection_get_alias(proj)){
			raxRemove(vctx->defined_identifiers, (unsigned char *)identifier, strlen(identifier), NULL);
		}
	}

	return VISITOR_CONTINUE;
}

// validate a WITH clause
static VISITOR_STRATEGY _Validate_WITH_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	vctx->clause = cypher_astnode_type(n);

	if(_Validate_LIMIT_SKIP_Modifiers(cypher_ast_with_get_limit(n),
		cypher_ast_with_get_skip(n)) == AST_INVALID) {
		return VISITOR_BREAK;
	}

	// manually traverse children. order by and predicate should be aware of the
	// vars introduced in the with projections, but the projections should not
	for(uint i = 0; i < cypher_ast_with_nprojections(n); i++) {
		// visit the projection
		const cypher_astnode_t *proj = cypher_ast_with_get_projection(n, i);
		AST_Visitor_visit(proj, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// introduce WITH aliases to the bound vars context
	if(!_AST_GetWithAliases(n, vctx->defined_identifiers)) {
		return VISITOR_BREAK;
	}

	// visit predicate clause
	const cypher_astnode_t *predicate = cypher_ast_with_get_predicate(n);
	if(predicate) {
		AST_Visitor_visit(predicate, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// visit ORDER BY clause
	const cypher_astnode_t *order_by = cypher_ast_with_get_order_by(n);
	if(order_by) {
		AST_Visitor_visit(order_by, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// if one of the 'projections' is a star -> proceed with current env
	// otherwise build a new environment using the new column names (aliases)
	if(!cypher_ast_with_has_include_existing(n)) {
		rax *projected_identifiers = raxNew();

		// introduce the WITH aliases to the bound vars context
		for(uint i = 0; i < cypher_ast_with_nprojections(n); i++) {
			const cypher_astnode_t *proj = cypher_ast_with_get_projection(n, i);
			const cypher_astnode_t *expr =
				cypher_ast_projection_get_expression(proj);
			const cypher_astnode_t *ast_alias =
				cypher_ast_projection_get_alias(proj);
			const char *identifier_name;
			void *identifier = NULL;

			if(ast_alias) {
				// Retrieve "a" from "WITH x AS a" or "WITH [1, 2, 3] AS a"
				identifier_name = cypher_ast_identifier_get_name(ast_alias);

				if(cypher_astnode_type(expr) == CYPHER_AST_IDENTIFIER) {
					// Retrieve "x" from "WITH x AS a"

					// to detect invalid queries like this:
					// MATCH (x) WITH x AS a MATCH ()-[a]->() RETURN 0
					// we need to do the following:
					// 1. retrieve "x" from the old environment with its
					// correct type (node)
					// 2. stored "a" in the new environment with the same
					// type of "x"
					const char *current_identifier_name =
						cypher_ast_identifier_get_name(expr);
					uint len = strlen(current_identifier_name);
					identifier = raxFind(vctx->defined_identifiers,
						  (unsigned char *)current_identifier_name, len);
				}
			} else {
				// Retrieve "x" from "WITH x"
				ast_alias = cypher_ast_projection_get_expression(proj);

				if(cypher_astnode_type(ast_alias) == CYPHER_AST_IDENTIFIER) {
					identifier_name = cypher_ast_identifier_get_name(ast_alias);
					uint len = strlen(identifier_name);
					identifier = raxFind(vctx->defined_identifiers,
						  (unsigned char *)identifier_name, len);
				}
			}

			raxInsert(projected_identifiers, (unsigned char *)identifier_name,
				strlen(identifier_name), identifier, NULL);
		}

		// free old env, set new one
		raxFree(vctx->defined_identifiers);
		vctx->defined_identifiers = projected_identifiers;
	}

	return VISITOR_CONTINUE;
}

// validate a DELETE clause
static VISITOR_STRATEGY _Validate_DELETE_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);
	uint expression_count = cypher_ast_delete_nexpressions(n);
	for(uint i = 0; i < expression_count; i++) {
		const cypher_astnode_t *exp = cypher_ast_delete_get_expression(n, i);
		cypher_astnode_type_t type = cypher_astnode_type(exp);
		// expecting an identifier or a function call
		// identifiers and calls that don't resolve to a node, path or edge
		// will raise an error at run-time
		if(type != CYPHER_AST_IDENTIFIER &&
				type != CYPHER_AST_APPLY_OPERATOR &&
				type != CYPHER_AST_APPLY_ALL_OPERATOR &&
				type != CYPHER_AST_SUBSCRIPT_OPERATOR) {
			ErrorCtx_SetError("DELETE can only be called on nodes, paths and relationships");
			return VISITOR_BREAK;
		}
	}

	return VISITOR_RECURSE;
}

// checks if a set property contains non-aliased references in its lhs
static VISITOR_STRATEGY _Validate_set_property
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);
	if(!start) {
		return VISITOR_CONTINUE;
	}

	const cypher_astnode_t *ast_prop = cypher_ast_set_property_get_property(n);
	const cypher_astnode_t *ast_entity = cypher_ast_property_operator_get_expression(ast_prop);
	if(cypher_astnode_type(ast_entity) != CYPHER_AST_IDENTIFIER) {
		ErrorCtx_SetError("RedisGraph does not currently support non-alias references on the left-hand side of SET expressions");
		return VISITOR_BREAK;
	}

	return VISITOR_RECURSE;
}

// validate a SET clause
static VISITOR_STRATEGY _Validate_SET_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);
	return VISITOR_RECURSE;
}

// validate a UNION clause
static VISITOR_STRATEGY _Validate_UNION_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	// make sure all UNIONs specify ALL or none of them do
	if(vctx->union_all == NOT_DEFINED) {
		vctx->union_all = cypher_ast_union_has_all(n);
	} else if(vctx->union_all != cypher_ast_union_has_all(n)) {
		ErrorCtx_SetError("Invalid combination of UNION and UNION ALL");
		return VISITOR_BREAK;
	}

	// free old bounded vars environment, create a new one
	vctx->clause = cypher_astnode_type(n);
	raxFree(vctx->defined_identifiers);
	vctx->defined_identifiers = raxNew();

	return VISITOR_RECURSE;
}

// validate a CREATE clause
static VISITOR_STRATEGY _Validate_CREATE_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);
	return VISITOR_RECURSE;
}

// validate a MERGE clause
static VISITOR_STRATEGY _Validate_MERGE_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);
	return VISITOR_RECURSE;
}

// validate a ON CREATE clause
static VISITOR_STRATEGY _Validate_ON_CREATE_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);
	return VISITOR_RECURSE;
}

// validate a ON MATCH clause
static VISITOR_STRATEGY _Validate_ON_MATCH_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);
	return VISITOR_RECURSE;
}

static VISITOR_STRATEGY _Validate_UNWIND_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);

	// introduce alias to bound vars
	const cypher_astnode_t *alias = cypher_ast_unwind_get_alias(n);
	const char *identifier = cypher_ast_identifier_get_name(alias);
	raxInsert(vctx->defined_identifiers, (unsigned char *)identifier, strlen(identifier), NULL, NULL);
	return VISITOR_RECURSE;
}

// validate a FOREACH clause
// MATCH (n) FOREACH(x in [1,2,3] | CREATE (n)-[:R]->({v:x}))
static VISITOR_STRATEGY _Validate_FOREACH_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = visitor->ctx;

	// we enter ONLY when start=true, so no check is needed

	// build a new environment of bounded vars from the current one to be
	// used in the traversal of the visitor in the clauses of the FOREACH
	// clause - as they are local to the FOREACH clause
	rax *orig_env = vctx->defined_identifiers;
	rax *scoped_env = raxClone(orig_env);
	vctx->defined_identifiers = scoped_env;

	// set the clause of the context
	vctx->clause = CYPHER_AST_FOREACH;

	// visit FOREACH array expression
	const cypher_astnode_t *list_node =
		cypher_ast_foreach_get_expression(n);
	AST_Visitor_visit(list_node, visitor);

	// introduce loop variable to bound vars
	const cypher_astnode_t *identifier_node =
		cypher_ast_foreach_get_identifier(n);

	const char *identifier =
		cypher_ast_identifier_get_name(identifier_node);

	raxInsert(vctx->defined_identifiers, (unsigned char *) identifier,
				strlen(identifier), NULL, NULL);

	// visit FOREACH loop body clauses
	uint nclauses = cypher_ast_foreach_nclauses(n);
	for(uint i = 0; i < nclauses; i++) {
		const cypher_astnode_t *clause = cypher_ast_foreach_get_clause(n, i);
		// make sure it is an updating clause
		cypher_astnode_type_t child_clause_type =
			cypher_astnode_type(clause);
		if(child_clause_type != CYPHER_AST_CREATE  &&
			child_clause_type != CYPHER_AST_SET     &&
			child_clause_type != CYPHER_AST_REMOVE  &&
			child_clause_type != CYPHER_AST_MERGE   &&
			child_clause_type != CYPHER_AST_DELETE  &&
			child_clause_type != CYPHER_AST_FOREACH
		) {
			ErrorCtx_SetError("Error: Only updating clauses may reside in FOREACH");
			break;
		}

		// visit the clause
		AST_Visitor_visit(clause, visitor);
	}

	// restore original environment of bounded vars
	vctx->defined_identifiers = orig_env;
	raxFree(scoped_env);

	// check for errors
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// do not traverse children
	return VISITOR_CONTINUE;
}

// validate a RETURN clause
static VISITOR_STRATEGY _Validate_RETURN_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	vctx->clause = cypher_astnode_type(n);
	uint num_return_projections = cypher_ast_return_nprojections(n);

	// visit LIMIT and SKIP
	if(_Validate_LIMIT_SKIP_Modifiers(cypher_ast_return_get_limit(n), cypher_ast_return_get_skip(n))
		== AST_INVALID) {
			return VISITOR_BREAK;
	}

	if(!cypher_ast_return_has_include_existing(n)) {
		// check for duplicate column names
		rax           *rax          = raxNew();
		const char   **columns      = AST_BuildReturnColumnNames(n);
		uint           column_count = array_len(columns);

		for (uint i = 0; i < column_count; i++) {
			// column with same name is invalid
			if(raxTryInsert(rax, (unsigned char *)columns[i], strlen(columns[i]), NULL, NULL) == 0) {
				ErrorCtx_SetError("Error: Multiple result columns with the same name are not supported");
				break;
			}
		}

		raxFree(rax);
		array_free(columns);
	}

	// manually traverse children. order by and predicate should be aware of the
	// vars introduced in the with projections, but the projections should not
	for(uint i = 0; i < cypher_ast_return_nprojections(n); i++) {
		// visit the projection
		const cypher_astnode_t *proj = cypher_ast_return_get_projection(n, i);
		AST_Visitor_visit(proj, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// introduce bound vars
	for(uint i = 0; i < num_return_projections; i ++) {
		const cypher_astnode_t *child = cypher_ast_return_get_projection(n, i);
		const cypher_astnode_t *alias_node = cypher_ast_projection_get_alias(child);
		if(alias_node == NULL) {
			continue;
		}
		const char *alias = cypher_ast_identifier_get_name(alias_node);
		raxInsert(vctx->defined_identifiers, (unsigned char *)alias, strlen(alias), NULL, NULL);
	}

	// visit ORDER BY clause
	const cypher_astnode_t *order_by = cypher_ast_return_get_order_by(n);
	if(order_by) {
		AST_Visitor_visit(order_by, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	// do not traverse children
	return !ErrorCtx_EncounteredError() ? VISITOR_CONTINUE : VISITOR_BREAK;
}

// validate a MATCH clause
static VISITOR_STRATEGY _Validate_MATCH_Clause
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	cypher_astnode_type_t backup_clause = vctx->clause;

	// visit pattern
	const cypher_astnode_t *pattern = cypher_ast_match_get_pattern(n);
	vctx->clause = CYPHER_AST_MATCH;
	AST_Visitor_visit(pattern, visitor);
	if(ErrorCtx_EncounteredError()) {
		return VISITOR_BREAK;
	}

	// visit predicate
	vctx->clause = backup_clause;
	const cypher_astnode_t *pred = cypher_ast_match_get_predicate(n);
	if(pred) {
		AST_Visitor_visit(pred, visitor);
		if(ErrorCtx_EncounteredError()) {
			return VISITOR_BREAK;
		}
	}

	vctx->clause = cypher_astnode_type(n);
	return VISITOR_CONTINUE;
}

// validate index creation
static VISITOR_STRATEGY _Validate_index_creation
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	validations_ctx *vctx = AST_Visitor_GetContext(visitor);

	if(!start) {
		return VISITOR_CONTINUE;
	}

	vctx->clause = cypher_astnode_type(n);

	const cypher_astnode_t *id = cypher_ast_create_pattern_props_index_get_identifier(n);
	const char *name = cypher_ast_identifier_get_name(id);
	raxInsert(vctx->defined_identifiers, (unsigned char *)name, strlen(name), NULL, NULL);
	return VISITOR_RECURSE;
}

// A query must end in a RETURN clause, a procedure, or an updating clause
// (CREATE, MERGE, DELETE, SET, REMOVE or FOREACH)
static AST_Validation _ValidateQueryTermination
(
	const AST *ast  // ast
) {
	ASSERT(ast != NULL);

	uint clause_idx = 0;
	const cypher_astnode_t *return_clause = NULL;
	const cypher_astnode_t *following_clause = NULL;
	uint clause_count = cypher_ast_query_nclauses(ast->root);

	// libcypher-parser does not enforce clause sequence order:
	// queries such as 'RETURN CREATE' and 'RETURN RETURN' are considered
	// valid by the parser
	// make sure the only clause following RETURN is UNION

	// get first instance of a RETURN clause
	return_clause = AST_GetClause(ast, CYPHER_AST_RETURN, &clause_idx);
	if(return_clause != NULL && clause_idx < clause_count - 1) {
		// RETURN clause isn't the last clause
		// the only clause which can follow a RETURN is the UNION clause
		following_clause = AST_GetClauseByIdx(ast, clause_idx + 1);
		if(cypher_astnode_type(following_clause) != CYPHER_AST_UNION) {
			// unexpected clause following RETURN
			ErrorCtx_SetError("Unexpected clause following RETURN");
			return AST_INVALID;
		}
	}

	const cypher_astnode_t *last_clause = cypher_ast_query_get_clause(ast->root, clause_count - 1);
	cypher_astnode_type_t type = cypher_astnode_type(last_clause);
	if(type != CYPHER_AST_RETURN   &&
	   type != CYPHER_AST_CREATE   &&
	   type != CYPHER_AST_MERGE    &&
	   type != CYPHER_AST_DELETE   &&
	   type != CYPHER_AST_SET      &&
	   type != CYPHER_AST_CALL     &&
	   type != CYPHER_AST_REMOVE   &&
	   type != CYPHER_AST_FOREACH
	  ) {
		ErrorCtx_SetError("Query cannot conclude with %s (must be RETURN or an update clause)",
						  cypher_astnode_typestr(type));
		return AST_INVALID;
	}
	return AST_VALID;
}

// default visit function
VISITOR_STRATEGY _default_visit
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	ASSERT(n != NULL);

	return VISITOR_RECURSE;
}

// Perform validations not constrained to a specific scope
static AST_Validation _ValidateQuerySequence
(
	const AST *ast
) {
	// Validate the final clause
	if(_ValidateQueryTermination(ast) != AST_VALID) {
		return AST_INVALID;
	}

	// The query cannot begin with a "WITH *" projection.
	const cypher_astnode_t *start_clause = cypher_ast_query_get_clause(ast->root, 0);
	if(cypher_astnode_type(start_clause) == CYPHER_AST_WITH &&
	   cypher_ast_with_has_include_existing(start_clause)) {
		ErrorCtx_SetError("Query cannot begin with 'WITH *'");
		return AST_INVALID;
	}

	// The query cannot begin with a "RETURN *" projection.
	if(cypher_astnode_type(start_clause) == CYPHER_AST_RETURN &&
	   cypher_ast_return_has_include_existing(start_clause)) {
		ErrorCtx_SetError("Query cannot begin with 'RETURN *'");
		return AST_INVALID;
	}

	return AST_VALID;
}

/* In any given query scope, reading clauses (MATCH, UNWIND, and InQueryCall)
 * cannot follow updating clauses (CREATE, MERGE, DELETE, SET, REMOVE, FOREACH).
 * https://s3.amazonaws.com/artifacts.opencypher.org/railroad/SinglePartQuery.html
 * Additionally, a MATCH clause cannot follow an OPTIONAL MATCH clause. */
static AST_Validation _ValidateClauseOrder
(
	const AST *ast  // ast
) {
	uint clause_count = cypher_ast_query_nclauses(ast->root);

	bool encountered_optional_match = false;
	bool encountered_updating_clause = false;
	for(uint i = 0; i < clause_count; i ++) {
		const cypher_astnode_t *clause = cypher_ast_query_get_clause(ast->root, i);
		cypher_astnode_type_t type = cypher_astnode_type(clause);
		encountered_updating_clause = (encountered_updating_clause   ||
									  (type == CYPHER_AST_CREATE     ||
									   type == CYPHER_AST_MERGE      ||
									   type == CYPHER_AST_DELETE     ||
									   type == CYPHER_AST_SET        ||
									   type == CYPHER_AST_REMOVE     ||
									   type == CYPHER_AST_FOREACH));
		if(encountered_updating_clause && (type == CYPHER_AST_MATCH  ||
										   type == CYPHER_AST_UNWIND ||
										   type == CYPHER_AST_CALL)) {
			ErrorCtx_SetError("A WITH clause is required to introduce %s after an updating clause",
							  cypher_astnode_typestr(type));
			return AST_INVALID;
		}

		if(type == CYPHER_AST_MATCH) {
			// Check whether this match is optional.
			bool current_clause_is_optional = cypher_ast_match_is_optional(clause);
			// If the current clause is non-optional but we have already encountered an optional match, emit an error.
			if(!current_clause_is_optional && encountered_optional_match) {
				ErrorCtx_SetError("A WITH clause is required to introduce a MATCH clause after an OPTIONAL MATCH");
				return AST_INVALID;
			}
			encountered_optional_match |= current_clause_is_optional;
		}

		if(type == CYPHER_AST_WITH) {
			encountered_optional_match = false;
			encountered_updating_clause = false;
		}
	}

	return AST_VALID;
}

// break visitor traversal, resulting in a fast-fold
static VISITOR_STRATEGY _visit_break
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	Error_UnsupportedASTNodeType(n);
	return VISITOR_BREAK;
}

// validate a binary operator, break if it is unsupported
static VISITOR_STRATEGY _Validate_binary_op
(
	const cypher_astnode_t *n,  // ast-node
	bool start,                 // first traversal
	ast_visitor *visitor        // visitor
) {
	const cypher_operator_t *op = cypher_ast_binary_operator_get_operator(n);
	if(op == CYPHER_OP_SUBSCRIPT ||
	   op == CYPHER_OP_MAP_PROJECTION ||
	   op == CYPHER_OP_REGEX) {
		Error_UnsupportedASTOperator(op);
		return VISITOR_BREAK;
	}
	return VISITOR_RECURSE;
}

// validate a query
static AST_Validation _ValidateScopes
(
	AST *ast  // ast
) {
	// create a context for the traversal
	validations_ctx ctx;
	ctx.union_all = NOT_DEFINED;
	ctx.defined_identifiers = raxNew();
	ctx.intermediate_identifiers = raxNew();

	// create a visitor
	ast_visitor visitor = {&ctx, validations_mapping};

	// visit (traverse) the ast
	AST_Visitor_visit(ast->root, &visitor);

	// cleanup
	raxFree(ctx.defined_identifiers);
	raxFree(ctx.intermediate_identifiers);

	return !ErrorCtx_EncounteredError() ? AST_VALID : AST_INVALID;
}

// build the global mapping from ast-node-type to visiting functions
bool AST_ValidationsMappingInit(void) {
	// create a mapping for the validations

	// set default entries
	for(uint i = 0; i < 114; i++) {
		validations_mapping[i] = _default_visit;
	}

	// populate the mapping with validation functions

	//--------------------------------------------------------------------------
	// register supported types
	//--------------------------------------------------------------------------

	validations_mapping[CYPHER_AST_SET]                        = _Validate_SET_Clause;
	validations_mapping[CYPHER_AST_MAP]                        = _Validate_map;
	validations_mapping[CYPHER_AST_ANY]                        = _Validate_list_comprehension;
	validations_mapping[CYPHER_AST_ALL]                        = _Validate_list_comprehension;
	validations_mapping[CYPHER_AST_CALL]                       = _Validate_CALL_Clause;
	validations_mapping[CYPHER_AST_WITH]                       = _Validate_WITH_Clause;
	validations_mapping[CYPHER_AST_NONE]                       = _Validate_list_comprehension;
	validations_mapping[CYPHER_AST_UNION]                      = _Validate_UNION_Clause;
	validations_mapping[CYPHER_AST_MATCH]                      = _Validate_MATCH_Clause;
	validations_mapping[CYPHER_AST_MERGE]                      = _Validate_MERGE_Clause;
	validations_mapping[CYPHER_AST_SINGLE]                     = _Validate_list_comprehension;
	validations_mapping[CYPHER_AST_RETURN]                     = _Validate_RETURN_Clause;
	validations_mapping[CYPHER_AST_UNWIND]                     = _Validate_UNWIND_Clause;
	validations_mapping[CYPHER_AST_CREATE]                     = _Validate_CREATE_Clause;
	validations_mapping[CYPHER_AST_DELETE]                     = _Validate_DELETE_Clause;
	validations_mapping[CYPHER_AST_REDUCE]                     = _Validate_reduce;
	validations_mapping[CYPHER_AST_FOREACH]                    = _Validate_FOREACH_Clause;
	validations_mapping[CYPHER_AST_IDENTIFIER]                 = _Validate_identifier;
	validations_mapping[CYPHER_AST_PROJECTION]                 = _Validate_projection;
	validations_mapping[CYPHER_AST_NAMED_PATH]                 = _Validate_named_path;
	validations_mapping[CYPHER_AST_REL_PATTERN]                = _Validate_rel_pattern;
	validations_mapping[CYPHER_AST_SET_PROPERTY]               = _Validate_set_property;
	validations_mapping[CYPHER_AST_NODE_PATTERN]               = _Validate_node_pattern;
	validations_mapping[CYPHER_AST_PATTERN_PATH]               = _Validate_pattern_path;
	validations_mapping[CYPHER_AST_SHORTEST_PATH]              = _Validate_shortest_path;
	validations_mapping[CYPHER_AST_APPLY_OPERATOR]             = _Validate_apply_operator;
	validations_mapping[CYPHER_AST_APPLY_ALL_OPERATOR]         = _Validate_apply_all_operator;
	validations_mapping[CYPHER_AST_LIST_COMPREHENSION]         = _Validate_list_comprehension;
	validations_mapping[CYPHER_AST_PATTERN_COMPREHENSION]      = _Validate_pattern_comprehension;
	validations_mapping[CYPHER_AST_CREATE_PATTERN_PROPS_INDEX] = _Validate_index_creation;
	validations_mapping[CYPHER_AST_ON_CREATE]                  = _Validate_ON_CREATE_Clause;
	validations_mapping[CYPHER_AST_ON_MATCH]                   = _Validate_ON_MATCH_Clause;
	validations_mapping[CYPHER_AST_BINARY_OPERATOR]            = _Validate_binary_op;
	validations_mapping[CYPHER_AST_PATTERN]                    = _Validate_pattern;

	//--------------------------------------------------------------------------
	// register unsupported types
	//--------------------------------------------------------------------------

	validations_mapping[CYPHER_AST_START]                       = _visit_break;
	validations_mapping[CYPHER_AST_FILTER]                      = _visit_break;
	validations_mapping[CYPHER_AST_EXTRACT]                     = _visit_break;
	validations_mapping[CYPHER_AST_COMMAND]                     = _visit_break;
	validations_mapping[CYPHER_AST_LOAD_CSV]                    = _visit_break;
	validations_mapping[CYPHER_AST_MATCH_HINT]                  = _visit_break;
	validations_mapping[CYPHER_AST_USING_JOIN]                  = _visit_break;
	validations_mapping[CYPHER_AST_USING_SCAN]                  = _visit_break;
	validations_mapping[CYPHER_AST_INDEX_NAME]                  = _visit_break;
	validations_mapping[CYPHER_AST_REL_ID_LOOKUP]               = _visit_break;
	validations_mapping[CYPHER_AST_ALL_RELS_SCAN]               = _visit_break;
	validations_mapping[CYPHER_AST_USING_INDEX]                 = _visit_break;
	validations_mapping[CYPHER_AST_START_POINT]                 = _visit_break;
	validations_mapping[CYPHER_AST_REMOVE_ITEM]                 = _visit_break;
	validations_mapping[CYPHER_AST_QUERY_OPTION]                = _visit_break;
	validations_mapping[CYPHER_AST_REL_INDEX_QUERY]             = _visit_break;
	validations_mapping[CYPHER_AST_EXPLAIN_OPTION]              = _visit_break;
	validations_mapping[CYPHER_AST_PROFILE_OPTION]              = _visit_break;
	validations_mapping[CYPHER_AST_SCHEMA_COMMAND]              = _visit_break;
	validations_mapping[CYPHER_AST_NODE_ID_LOOKUP]              = _visit_break;
	validations_mapping[CYPHER_AST_ALL_NODES_SCAN]              = _visit_break;
	validations_mapping[CYPHER_AST_REL_INDEX_LOOKUP]            = _visit_break;
	validations_mapping[CYPHER_AST_NODE_INDEX_QUERY]            = _visit_break;
	validations_mapping[CYPHER_AST_NODE_INDEX_LOOKUP]           = _visit_break;
	validations_mapping[CYPHER_AST_USING_PERIODIC_COMMIT]       = _visit_break;
	validations_mapping[CYPHER_AST_DROP_REL_PROP_CONSTRAINT]    = _visit_break;
	validations_mapping[CYPHER_AST_DROP_NODE_PROP_CONSTRAINT]   = _visit_break;
	validations_mapping[CYPHER_AST_CREATE_REL_PROP_CONSTRAINT]  = _visit_break;
	validations_mapping[CYPHER_AST_CREATE_NODE_PROP_CONSTRAINT] = _visit_break;

	return true;
}

// Checks to see if libcypher-parser reported any errors.
bool AST_ContainsErrors
(
	const cypher_parse_result_t *result  // parse-result checked for errors
) {
	return cypher_parse_result_nerrors(result) > 0;
}

/* This function checks for the existence a valid root in the query.
 * As cypher_parse_result_t can have multiple roots such as comments,
 * only a query that has a root with type CYPHER_AST_STATEMENT is considered valid.
 * Comment roots are ignored. */
AST_Validation AST_Validate_ParseResultRoot
(
	const cypher_parse_result_t *result,
	int *index
) {
	// Check for failures in libcypher-parser
	ASSERT(AST_ContainsErrors(result) == false);

	uint nroots = cypher_parse_result_nroots(result);
	for(uint i = 0; i < nroots; i++) {
		const cypher_astnode_t *root = cypher_parse_result_get_root(result, i);
		cypher_astnode_type_t root_type = cypher_astnode_type(root);
		if(root_type == CYPHER_AST_LINE_COMMENT || root_type == CYPHER_AST_BLOCK_COMMENT ||
		   root_type == CYPHER_AST_COMMENT) {
			continue;
		} else if(root_type != CYPHER_AST_STATEMENT) {
			ErrorCtx_SetError("Encountered unsupported query type '%s'", cypher_astnode_typestr(root_type));
			return AST_INVALID;
		} else {
			// We got a statement.
			*index = i;
			return AST_VALID;
		}
	}

	// query with no roots like ';'
	if(nroots == 0) {
		ErrorCtx_SetError("Error: empty query");
	}

	return AST_INVALID;
}

// validate a query
AST_Validation AST_Validate_Query
(
	const cypher_astnode_t *root  // query to validate
) {
	const cypher_astnode_t *body = cypher_ast_statement_get_body(root);
	AST ast; // Build a fake AST with the correct AST root
	ast.root = body;

	cypher_astnode_type_t body_type = cypher_astnode_type(body);
	if(body_type == CYPHER_AST_CREATE_NODE_PROPS_INDEX    ||
	   body_type == CYPHER_AST_CREATE_PATTERN_PROPS_INDEX ||
	   body_type == CYPHER_AST_DROP_PROPS_INDEX) {
		return _ValidateScopes(&ast);
	}

	// Verify that the RETURN clause and terminating clause do not violate scoping rules.
	if(_ValidateQuerySequence(&ast) != AST_VALID) {
		return AST_INVALID;
	}

	// Verify that the clause order in the scope is valid.
	if(_ValidateClauseOrder(&ast) != AST_VALID) {
		return AST_INVALID;
	}

	// Verify that the clauses surrounding UNION return the same column names.
	if(_ValidateUnion_Clauses(&ast) != AST_VALID) {
		return AST_INVALID;
	}

	// validate positions of allShortestPaths
	if(!_ValidateAllShortestPaths(body)) {
		ErrorCtx_SetError("RedisGraph support allShortestPaths only in match clauses");
		return AST_INVALID;
	}

	if(!_ValidateShortestPaths(body)) {
		ErrorCtx_SetError("RedisGraph currently only supports shortestPath in WITH or RETURN clauses");
		return AST_INVALID;
	}

	// Check for invalid queries not captured by libcypher-parser
	return _ValidateScopes(&ast);
}

//------------------------------------------------------------------------------
// Query params validations
//------------------------------------------------------------------------------

// validate the parameters of a statement expression are not EXPLAIN or PROFILE
static AST_Validation _ValidateParamsOnly
(
	const cypher_astnode_t *statement  // statement
) {
	uint noptions = cypher_ast_statement_noptions(statement);
	for(uint i = 0; i < noptions; i++) {
		const cypher_astnode_t *option = cypher_ast_statement_get_option(statement, i);
		const cypher_astnode_type_t type = cypher_astnode_type(option);
		if((type == CYPHER_AST_EXPLAIN_OPTION) || (type == CYPHER_AST_PROFILE_OPTION)) {
			const char *invalid_option_name = cypher_astnode_typestr(type);
			ErrorCtx_SetError("Please use GRAPH.%s 'key' 'query' command instead of GRAPH.QUERY 'key' '%s query'",
							  invalid_option_name, invalid_option_name);
			return AST_INVALID;
		}
	}
	return AST_VALID;
}

// Make sure no duplicated parameters are given in a statement expression
static AST_Validation _ValidateDuplicateParameters
(
	const cypher_astnode_t *statement  // statement
) {
	rax *param_names = raxNew();
	uint noptions = cypher_ast_statement_noptions(statement);
	for(uint i = 0; i < noptions; i++) {
		const cypher_astnode_t *option = cypher_ast_statement_get_option(statement, i);
		uint nparams = cypher_ast_cypher_option_nparams(option);
		for(uint j = 0; j < nparams; j++) {
			const cypher_astnode_t *param = cypher_ast_cypher_option_get_param(option, j);
			const char *paramName = cypher_ast_string_get_value(cypher_ast_cypher_option_param_get_name(param));
			// If parameter already exists return an error.
			if(!raxInsert(param_names, (unsigned char *) paramName, strlen(paramName), NULL, NULL)) {
				ErrorCtx_SetError("Duplicated parameter: %s", paramName);
				raxFree(param_names);
				return AST_INVALID;
			}
		}
	}
	raxFree(param_names);
	return AST_VALID;
}

// validate parameters of a query
AST_Validation AST_Validate_QueryParams
(
	const cypher_parse_result_t *result  // parse-result to validate
) {
	int index;
	if(AST_Validate_ParseResultRoot(result, &index) != AST_VALID) {
		return AST_INVALID;
	}

	const cypher_astnode_t *root = cypher_parse_result_get_root(result, index);

	// in case of no parameters
	if(cypher_ast_statement_noptions(root) == 0) {
		return AST_VALID;
	}

	if(_ValidateParamsOnly(root) != AST_VALID) {
		return AST_INVALID;
	}
	if(_ValidateDuplicateParameters(root) != AST_VALID) {
		return AST_INVALID;
	}

	// create a context for the traversal
	validations_ctx ctx;
	ctx.union_all = NOT_DEFINED;
	ctx.defined_identifiers = raxNew();

	// create a visitor
	ast_visitor visitor = {&ctx, validations_mapping};

	// visit (traverse) the ast
	AST_Visitor_visit(root, &visitor);

	// cleanup
	raxFree(ctx.defined_identifiers);

	return !ErrorCtx_EncounteredError() ? AST_VALID : AST_INVALID;
}

// report encountered errors by libcypher-parser
void AST_ReportErrors
(
	const cypher_parse_result_t *result  // parse-result
) {
	ASSERT(cypher_parse_result_nerrors(result) > 0);

	// report first encountered error
	const cypher_parse_error_t *error =
		cypher_parse_result_get_error(result, 0);

	// Get the position of an error.
	struct cypher_input_position errPos = cypher_parse_error_position(error);

	// Get the error message of an error.
	const char *errMsg = cypher_parse_error_message(error);

	// Get the error context of an error.
	// This returns a pointer to a null-terminated string, which contains a
	// section of the input around where the error occurred, that is limited
	// in length and suitable for presentation to a user.
	const char *errCtx = cypher_parse_error_context(error);

	// Get the offset into the context of an error.
	// Identifies the point of the error within the context string, allowing
	// this to be reported to the user, typically with an arrow pointing to the
	// invalid character.
	size_t errCtxOffset = cypher_parse_error_context_offset(error);
	ErrorCtx_SetError("errMsg: %s line: %u, column: %u, offset: %zu errCtx: %s errCtxOffset: %zu",
					  errMsg, errPos.line, errPos.column, errPos.offset, errCtx, errCtxOffset);
}
