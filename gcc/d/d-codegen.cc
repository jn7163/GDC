// d-codegen.cc -- D frontend for GCC.
// Copyright (C) 2011, 2012 Free Software Foundation, Inc.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#include "d-system.h"
#include "d-lang.h"
#include "d-codegen.h"

#include "template.h"
#include "init.h"
#include "symbol.h"
#include "id.h"

IRState gen;

Module *current_module;
IRState *current_irs;
ObjectFile *object_file;


// Public routine called from D frontend to hide from glue interface.
// Returns TRUE if all templates are being emitted, either publicly
// or privately, into the current compilation.

bool
d_gcc_force_templates (void)
{
  return gen.emitTemplates == TEprivate;
}

// Return the DECL_CONTEXT for symbol D_SYM.

tree
IRState::declContext (Dsymbol *d_sym)
{
  Dsymbol *orig_sym = d_sym;
  AggregateDeclaration *ad;

  while ((d_sym = d_sym->toParent2()))
    {
      if (d_sym->isFuncDeclaration())
	{
	  // dwarf2out chokes without this check... (output_pubnames)
	  FuncDeclaration *f = orig_sym->isFuncDeclaration();
	  if (f && !gen.functionNeedsChain (f))
	    return NULL_TREE;

	  return d_sym->toSymbol()->Stree;
	}
      else if ((ad = d_sym->isAggregateDeclaration()))
	{
	  tree ctx = ad->type->toCtype();
	  if (ad->isClassDeclaration())
	    {
	      // RECORD_TYPE instead of REFERENCE_TYPE
	      ctx = TREE_TYPE (ctx);
	    }

	  return ctx;
	}
      else if (d_sym->isModule())
	{
	  return d_sym->toSymbol()->ScontextDecl;
	}
    }
  return NULL_TREE;
}

// Update current source file location to LOC.

void
IRState::doLineNote (const Loc& loc)
{
  ObjectFile::doLineNote (loc);
}

// Add local variable V into the current body.  If NO_INIT,
// then variable does not have a default initialiser.

void
IRState::emitLocalVar (VarDeclaration *v, bool no_init)
{
  if (v->isDataseg() || v->isMember())
    return;

  Symbol *sym = v->toSymbol();
  tree var_decl = sym->Stree;

  gcc_assert (!TREE_STATIC (var_decl));
  pushdecl (var_decl);

  if (TREE_CODE (var_decl) == CONST_DECL)
    return;

  DECL_CONTEXT (var_decl) = getLocalContext();

  // Compiler generated symbols
  if (v == this->func->vresult || v == this->func->v_argptr
      || v == this->func->v_arguments_var)
    DECL_ARTIFICIAL (var_decl) = 1;

  tree var_exp;
  if (sym->SframeField)
    {
      // Fixes debugging local variables.
      SET_DECL_VALUE_EXPR (var_decl, var (v));
      DECL_HAS_VALUE_EXPR_P (var_decl) = 1;
    }
  var_exp = var_decl;

  // complete initializer expression (include MODIFY_EXPR, e.g.)
  tree init_exp = NULL_TREE;
  tree init_val = NULL_TREE;

  if (!no_init && !DECL_INITIAL (var_decl) && v->init)
    {
      if (!v->init->isVoidInitializer())
	{
	  ExpInitializer *exp_init = v->init->isExpInitializer();
	  Expression *ie = exp_init->toExpression();
	  init_exp = ie->toElem (this);
	}
      else
	no_init = true;
    }
  else
    gcc_assert (v->init == NULL);

  if (!no_init)
    {
      object_file->doLineNote (v->loc);

      if (!init_val)
	{
	  init_val = DECL_INITIAL (var_decl);
	  DECL_INITIAL (var_decl) = NULL_TREE; // %% from expandDecl
	}
      if (!init_exp && init_val)
	init_exp = build_vinit (var_exp, init_val);

      if (init_exp)
	addExp (init_exp);
      else if (!init_val && v->size (v->loc)) // Zero-length arrays do not have an initializer
	warning (OPT_Wuninitialized, "uninitialized variable '%s'", v->ident ? v->ident->string : "(no name)");
    }
}

// Return an unnamed local temporary of type T_TYPE.

tree
IRState::localVar (tree t_type)
{
  tree t_decl = build_decl (BUILTINS_LOCATION, VAR_DECL, NULL_TREE, t_type);
  DECL_CONTEXT (t_decl) = getLocalContext();
  DECL_ARTIFICIAL (t_decl) = 1;
  DECL_IGNORED_P (t_decl) = 1;
  pushdecl (t_decl);
  return t_decl;
}

// Return an unnamed local temporary of type E_TYPE.

tree
IRState::localVar (Type *e_type)
{
  return localVar (e_type->toCtype());
}

// Return an undeclared local temporary of type T_TYPE
// for use with BIND_EXPR.

tree
IRState::exprVar (tree t_type)
{
  tree t_decl = build_decl (BUILTINS_LOCATION, VAR_DECL, NULL_TREE, t_type);
  DECL_CONTEXT (t_decl) = getLocalContext();
  DECL_ARTIFICIAL (t_decl) = 1;
  DECL_IGNORED_P (t_decl) = 1;
  layout_decl (t_decl, 0);
  return t_decl;
}

// Return an undeclared local temporary OUT_VAR initialised
// with result of expression EXP.

tree
IRState::maybeExprVar (tree exp, tree *out_var)
{
  tree t = exp;

  // Get the base component.
  while (TREE_CODE (t) == COMPONENT_REF)
    t = TREE_OPERAND (t, 0);

  if (!DECL_P (t) && !REFERENCE_CLASS_P (t))
    {
      *out_var = exprVar (TREE_TYPE (exp));
      DECL_INITIAL (*out_var) = exp;
      return *out_var;
    }
  else
    {
      *out_var = NULL_TREE;
      return exp;
    }
}

// Emit an INIT_EXPR for decl T_DECL.

void
IRState::expandDecl (tree t_decl)
{
  // nothing, pushdecl will add t_decl to a BIND_EXPR
  if (DECL_INITIAL (t_decl))
    {
      tree exp = build_vinit (t_decl, DECL_INITIAL (t_decl));
      addExp (exp);
      DECL_INITIAL (t_decl) = NULL_TREE;
    }
}

// Return the correct decl to be used for variable V.
// Could be a VAR_DECL, or a FIELD_DECL from a closure.

tree
IRState::var (Declaration *decl)
{
  VarDeclaration *v = decl->isVarDeclaration();

  if (v && v->toSymbol()->SframeField != NULL_TREE)
    {
      FuncDeclaration *f = v->toParent2()->isFuncDeclaration();
      tree cf = getFrameRef (f);
      tree field = v->toSymbol()->SframeField;

      gcc_assert (field != NULL_TREE);
      return component_ref (build_deref (cf), field);
    }
  else
    {
      // Static var or auto var that the back end will handle for us
      return decl->toSymbol()->Stree;
    }
}

// Return expression EXP, whose type has been converted to TYPE.

tree
IRState::convertTo (tree type, tree exp)
{
  // Check this first before passing to build_dtype.
  if (error_mark_p (type) || error_mark_p (TREE_TYPE (exp)))
    return error_mark_node;

  Type *target_type = build_dtype (type);
  Type *expr_type = build_dtype (TREE_TYPE (exp));

  if (target_type && expr_type)
    return convertTo (exp, expr_type, target_type);

  return convert (type, exp);
}

// Return a TREE representation of EXP implictly converted to TARGET_TYPE.

tree
IRState::convertTo (Expression *exp, Type *target_type)
{
  return convertTo (exp->toElem (this), exp->type, target_type);
}

// Return expression EXP, whose type has been convert from EXP_TYPE to TARGET_TYPE.

tree
IRState::convertTo (tree exp, Type *exp_type, Type *target_type)
{
  tree result = NULL_TREE;

  gcc_assert (exp_type && target_type);
  Type *ebtype = exp_type->toBasetype();
  Type *tbtype = target_type->toBasetype();

  if (d_types_same (exp_type, target_type))
    return exp;

  if (error_mark_p (exp))
    return exp;

  switch (ebtype->ty)
    {
    case Tdelegate:
      if (tbtype->ty == Tdelegate)
	{
	  exp = maybe_make_temp (exp);
	  return build_delegate_cst (delegate_method (exp), delegate_object (exp),
				     target_type);
	}
      else if (tbtype->ty == Tpointer)
	{
	  // The front-end converts <delegate>.ptr to cast (void *)<delegate>.
	  // Maybe should only allow void* ?
	  exp = delegate_object (exp);
	}
      else
	{
	  ::error ("can't convert a delegate expression to %s", target_type->toChars());
	  return error_mark_node;
	}
      break;

    case Tstruct:
      if (tbtype->ty == Tstruct)
      {
	if (target_type->size() == exp_type->size())
	  {
	    // Allowed to cast to structs with same type size.
	    result = build_vconvert (target_type->toCtype(), exp);
	  }
	else if (tbtype->ty == Taarray)
	  {
	    tbtype = ((TypeAArray *)tbtype)->getImpl()->type;
	    return convertTo (exp, exp_type, tbtype);
	  }
	else
	  {
	    ::error ("can't convert struct %s to %s", exp_type->toChars(), target_type->toChars());
	    return error_mark_node;
	  }
      }
      // else, default conversion, which should produce an error
      break;

    case Tclass:
      if (tbtype->ty == Tclass)
      {
	ClassDeclaration *target_class_decl = ((TypeClass *) tbtype)->sym;
	ClassDeclaration *obj_class_decl = ((TypeClass *) ebtype)->sym;
	bool use_dynamic = false;
	int offset;

	if (target_class_decl->isBaseOf (obj_class_decl, &offset))
	  {
	    // Casting up the inheritance tree: Don't do anything special.
	    // Cast to an implemented interface: Handle at compile time.
	    if (offset == OFFSET_RUNTIME)
	      use_dynamic = true;
	    else if (offset)
	      {
		tree t = target_type->toCtype();
		exp = maybe_make_temp (exp);
		return build3 (COND_EXPR, t,
			       build_boolop (NE_EXPR, exp, d_null_pointer),
			       build_nop (t, build_offset (exp, size_int (offset))),
			       build_nop (t, d_null_pointer));
	      }
	    else
	      {
		// d_convert will make a NOP cast
		break;
	      }
	  }
	else if (target_class_decl == obj_class_decl)
	  {
	    // d_convert will make a NOP cast
	    break;
	  }
	else if (!obj_class_decl->isCOMclass())
	  use_dynamic = true;

	if (use_dynamic)
	  {
	    // Otherwise, do dynamic cast
	    tree args[2];
	    args[0] = exp;
	    args[1] = build_address (target_class_decl->toSymbol()->Stree);
	    return build_libcall (obj_class_decl->isInterfaceDeclaration()
				  ? LIBCALL_INTERFACE_CAST : LIBCALL_DYNAMIC_CAST, 2, args);
	  }
	else
	  {
	    warning (OPT_Wcast_result, "cast to %s will produce null result", target_type->toChars());
	    result = convertTo (target_type->toCtype(), d_null_pointer);
	    if (TREE_SIDE_EFFECTS (exp))
	      {
		// make sure the expression is still evaluated if necessary
		result = compound_expr (exp, result);
	      }
	    return result;
	  }
      }
      // else default conversion
      break;

    case Tsarray:
      if (tbtype->ty == Tpointer)
	{
	  result = build_nop (target_type->toCtype(), build_address (exp));
	}
      else if (tbtype->ty == Tarray)
	{
	  dinteger_t dim = ((TypeSArray *) ebtype)->dim->toInteger();
	  dinteger_t esize = ebtype->nextOf()->size();
	  dinteger_t tsize = tbtype->nextOf()->size();

	  tree ptrtype = tbtype->nextOf()->pointerTo()->toCtype();

	  if ((dim * esize) % tsize != 0)
	    {
	      ::error ("cannot cast %s to %s since sizes don't line up",
		       exp_type->toChars(), target_type->toChars());
	      return error_mark_node;
	    }
	  dim = (dim * esize) / tsize;

	  // Assumes casting to dynamic array of same type or void
	  return d_array_value (target_type->toCtype(),
				size_int (dim), build_nop (ptrtype, build_address (exp)));
	}
      else if (tbtype->ty == Tsarray)
	{
	  // D apparently allows casting a static array to any static array type
	  return build_vconvert (target_type->toCtype(), exp);
	}
      else if (tbtype->ty == Tstruct)
	{
	  // And allows casting a static array to any struct type too.
	  // %% type sizes should have already been checked by the frontend.
	  gcc_assert (target_type->size() == exp_type->size());
	  result = build_vconvert (target_type->toCtype(), exp);
	}
      else
	{
	  ::error ("cannot cast expression of type %s to type %s",
		   exp_type->toChars(), target_type->toChars());
	  return error_mark_node;
	}
      break;

    case Tarray:
      if (tbtype->ty == Tpointer)
	{
	  return convertTo (target_type->toCtype(), d_array_ptr (exp));
	}
      else if (tbtype->ty == Tarray)
	{
	  // assume tvoid->size() == 1
	  Type *src_elem_type = ebtype->nextOf()->toBasetype();
	  Type *dst_elem_type = tbtype->nextOf()->toBasetype();
	  d_uns64 sz_src = src_elem_type->size();
	  d_uns64 sz_dst = dst_elem_type->size();

	  if (/*src_elem_type->ty == Tvoid ||*/ sz_src == sz_dst)
	    {
	      // Convert from void[] or elements are the same size -- don't change length
	      return build_vconvert (target_type->toCtype(), exp);
	    }
	  else
	    {
	      unsigned mult = 1;
	      tree args[3];
	      args[0] = build_integer_cst (sz_dst, Type::tsize_t->toCtype());
	      args[1] = build_integer_cst (sz_src * mult, Type::tsize_t->toCtype());
	      args[2] = exp;
	      return build_libcall (LIBCALL_ARRAYCAST, 3, args, target_type->toCtype());
	    }
	}
      else if (tbtype->ty == Tsarray)
	{
	  // %% Strings are treated as dynamic arrays D2.
	  if (ebtype->isString() && tbtype->isString())
	    return indirect_ref (target_type->toCtype(), d_array_ptr (exp));
	}
      else
	{
	  ::error ("cannot cast expression of type %s to %s",
		   exp_type->toChars(), target_type->toChars());
	  return error_mark_node;
	}
      break;

    case Taarray:
      if (tbtype->ty == Taarray)
	return build_vconvert (target_type->toCtype(), exp);
      else if (tbtype->ty == Tstruct)
	{
	  ebtype = ((TypeAArray *)ebtype)->getImpl()->type;
	  return convertTo (exp, ebtype, target_type);
	}
      // Can convert associative arrays to void pointers.
      else if (tbtype == Type::tvoidptr)
	return build_vconvert (target_type->toCtype(), exp);
      // else, default conversion, which should product an error
      break;

    case Tpointer:
      // Can convert void pointers to associative arrays too...
      if (tbtype->ty == Taarray && ebtype == Type::tvoidptr)
	return build_vconvert (target_type->toCtype(), exp);
      break;

    case Tnull:
      if (tbtype->ty == Tarray)
	{
	  tree ptrtype = tbtype->nextOf()->pointerTo()->toCtype();
	  return d_array_value (target_type->toCtype(),
				size_int (0), build_nop (ptrtype, exp));
	}
      break;

    case Tvector:
      if (tbtype->ty == Tsarray)
	{
	  if (tbtype->size() == ebtype->size())
	    return build_vconvert (target_type->toCtype(), exp);
	}
      break;

    default:
      exp = fold_convert (exp_type->toCtype(), exp);
      gcc_assert (TREE_CODE (exp) != STRING_CST);
      break;
    }

  return result ? result :
    convert (target_type->toCtype(), exp);
}


// Apply semantics of assignment to a values of type TARGET_TYPE to EXPR
// (e.g., pointer = array -> pointer = &array[0])

// Return a TREE representation of EXPR implictly converted to TARGET_TYPE
// for use in assignment expressions MODIFY_EXPR, INIT_EXPR...

tree
IRState::convertForAssignment (Expression *expr, Type *target_type)
{
  Type *exp_base_type = expr->type->toBasetype();
  Type *target_base_type = target_type->toBasetype();
  tree exp_tree = NULL_TREE;

  // Assuming this only has to handle converting a non Tsarray type to
  // arbitrarily dimensioned Tsarrays.
  if (target_base_type->ty == Tsarray)
    {
      Type *sa_elem_type = target_base_type->nextOf()->toBasetype();

      while (sa_elem_type->ty == Tsarray)
	sa_elem_type = sa_elem_type->nextOf()->toBasetype();

      if (d_types_compatible (sa_elem_type, exp_base_type))
	{
	  // %% what about implicit converions...?
	  TypeSArray *sa_type = (TypeSArray *) target_base_type;
	  uinteger_t count = sa_type->dim->toUInteger();

	  tree ctor = build_constructor (target_type->toCtype(), 0);
	  if (count)
	    {
	      CtorEltMaker ce;
	      ce.cons (build2 (RANGE_EXPR, Type::tsize_t->toCtype(),
			       integer_zero_node, build_integer_cst (count - 1)),
		       object_file->stripVarDecl (convertForAssignment (expr, sa_type->next)));
	      CONSTRUCTOR_ELTS (ctor) = ce.head;
	    }
	  TREE_READONLY (ctor) = 1;
	  TREE_CONSTANT (ctor) = 1;
	  return ctor;
	}
    }

  if (!target_type->isscalar() && exp_base_type->isintegral())
    {
      // D Front end uses IntegerExp (0) to mean zero-init a structure
      // This could go in convert for assignment, but we only see this for
      // internal init code -- this also includes default init for _d_newarrayi...
      if (expr->toInteger() == 0)
	{
	  tree empty = build_constructor (target_type->toCtype(), NULL);
	  TREE_CONSTANT (empty) = 1;
	  TREE_STATIC (empty) = 1;
	  return empty;
	}

      gcc_unreachable();
    }

  exp_tree = expr->toElem (this);
  return convertForAssignment (exp_tree, expr->type, target_type);
}

// Return expression EXPR, whose type has been convert from EXPR_TYPE to TARGET_TYPE.

tree
IRState::convertForAssignment (tree expr, Type *expr_type, Type *target_type)
{
  return convertTo (expr, expr_type, target_type);
}

// Return a TREE representation of EXPR converted to represent parameter type ARG.

tree
IRState::convertForArgument (Expression *expr, Parameter *arg)
{
  if (arg_reference_p (arg))
    {
      tree exp_tree = this->toElemLvalue (expr);
      // front-end already sometimes automatically takes the address
      // TODO: Make this safer?  Can this be confused by a non-zero SymOff?
      if (expr->op != TOKaddress && expr->op != TOKsymoff && expr->op != TOKadd)
	exp_tree = build_address (exp_tree);

      return convert (type_passed_as (arg), exp_tree);
    }
  else
    {
      // Lazy arguments: expr should already be a delegate
      return expr->toElem (this);
    }
}

// Return a TREE representation of EXPR implictly converted to
// BOOLEAN_TYPE for use in conversion expressions.

tree
IRState::convertForCondition (Expression *expr)
{
  return convertForCondition (expr->toElem (this), expr->type);
}

// Perform default promotions for data used in expressions.
// Arrays and functions are converted to pointers;
// enumeral types or short or char, to int.
// In addition, manifest constants symbols are replaced by their values.

// Return truth-value conversion of expression EXPR from value type EXP_TYPE.

tree
IRState::convertForCondition (tree exp_tree, Type *exp_type)
{
  tree result = NULL_TREE;
  tree obj, func, tmp;

  switch (exp_type->toBasetype()->ty)
    {
    case Taarray:
      // Shouldn't this be...
      //  result = build_libcall (LIBCALL_AALEN, 1, &exp_tree);
      result = component_ref (exp_tree, TYPE_FIELDS (TREE_TYPE (exp_tree)));
      break;

    case Tarray:
      // Checks (length || ptr) (i.e ary !is null)
      tmp = maybe_make_temp (exp_tree);
      obj = delegate_object (tmp);
      func = delegate_method (tmp);
      if (TYPE_MODE (TREE_TYPE (obj)) == TYPE_MODE (TREE_TYPE (func)))
	{
	  result = build2 (BIT_IOR_EXPR, TREE_TYPE (obj), obj,
			   convertTo (TREE_TYPE (obj), func));
	}
      else
	{
	  obj = d_truthvalue_conversion (obj);
	  func = d_truthvalue_conversion (func);
	  // probably not worth using TRUTH_OROR ...
	  result = build2 (TRUTH_OR_EXPR, TREE_TYPE (obj), obj, func);
	}
      break;

    case Tdelegate:
      // Checks (function || object), but what good is it
      // if there is a null function pointer?
      if (D_METHOD_CALL_EXPR (exp_tree))
	extract_from_method_call (exp_tree, obj, func);
      else
	{
	  tmp = maybe_make_temp (exp_tree);
	  obj = delegate_object (tmp);
	  func = delegate_method (tmp);
	}
      obj = d_truthvalue_conversion (obj);
      func = d_truthvalue_conversion (func);
      // probably not worth using TRUTH_ORIF ...
      result = build2 (BIT_IOR_EXPR, TREE_TYPE (obj), obj, func);
      break;

    default:
      result = exp_tree;
      break;
    }

  return d_truthvalue_conversion (result);
}


// Convert EXP to a dynamic array.
// EXP must be a static array or dynamic array.

tree
IRState::toDArray (Expression *exp)
{
  TY ty = exp->type->toBasetype()->ty;
  tree val;
  if (ty == Tsarray)
    val = convertTo (exp, exp->type->toBasetype()->nextOf()->arrayOf());
  else if (ty == Tarray)
    val = exp->toElem (this);
  else
    {
      gcc_assert (ty == Tsarray || ty == Tarray);
      return NULL_TREE;
    }
  return val;
}

// Return TRUE if declaration DECL is a reference type.

bool
decl_reference_p (Declaration *decl)
{
  Type *base_type = decl->type->toBasetype();

  if (base_type->ty == Treference)
    return true;

  if (decl->storage_class & (STCout | STCref))
    return true;

  return false;
}

// Returns the real type for declaration DECL.
// Reference decls are converted to reference-to-types.
// Lazy decls are converted into delegates.

tree
declaration_type (Declaration *decl)
{
  tree decl_type = decl->type->toCtype();

  if (decl_reference_p (decl))
    decl_type = build_reference_type (decl_type);
  else if (decl->storage_class & STClazy)
    {
      TypeFunction *tf = new TypeFunction (NULL, decl->type, false, LINKd);
      TypeDelegate *t = new TypeDelegate (tf);
      decl_type = t->merge()->toCtype();
    }

  return decl_type;
}

// These should match the Declaration versions above
// Return TRUE if parameter ARG is a reference type.

bool
arg_reference_p (Parameter *arg)
{
  Type *base_type = arg->type->toBasetype();

  if (base_type->ty == Treference)
    return true;

  if (arg->storageClass & (STCout | STCref))
    return true;

  return false;
}

// Returns the real type for parameter ARG.
// Reference parameters are converted to reference-to-types.
// Lazy parameters are converted into delegates.

tree
type_passed_as (Parameter *arg)
{
  tree arg_type = arg->type->toCtype();
  if (arg_reference_p (arg))
    arg_type = build_reference_type (arg_type);
  else if (arg->storageClass & STClazy)
    {
      TypeFunction *tf = new TypeFunction (NULL, arg->type, false, LINKd);
      TypeDelegate *t = new TypeDelegate (tf);
      arg_type = t->merge()->toCtype();
    }
  return arg_type;
}

// Returns an array of type TYPE_NODE which has SIZE number of elements.

tree
d_array_type (Type *d_type, uinteger_t size)
{
  tree index_type_node;
  tree type_node = d_type->toCtype();

  if (size > 0)
    {
      index_type_node = size_int (size - 1);
      index_type_node = build_index_type (index_type_node);
    }
  else
    index_type_node = build_range_type (sizetype, size_zero_node,
					NULL_TREE);

  tree array_type = build_array_type (type_node, index_type_node);
  if (size == 0)
    {
      TYPE_SIZE (array_type) = bitsize_zero_node;
      TYPE_SIZE_UNIT (array_type) = size_zero_node;
    }
  return array_type;
}

// Appends the type attribute ATTRNAME with value VALUE onto type TYPE.

tree
insert_type_attribute (tree type, const char *attrname, tree value)
{
  tree attrib;
  tree ident = get_identifier (attrname);

  if (value)
    value = tree_cons (NULL_TREE, value, NULL_TREE);

  // types built by functions in tree.c need to be treated as immutabl
  if (!TYPE_ATTRIBUTES (type))
    type = build_variant_type_copy (type);

  attrib = tree_cons (ident, value, NULL_TREE);
  TYPE_ATTRIBUTES (type) = merge_attributes (TYPE_ATTRIBUTES (type), attrib);

  return type;
}

// Appends the decl attribute ATTRNAME with value VALUE onto decl DECL.

void
insert_decl_attribute (tree decl, const char *attrname, tree value)
{
  tree attrib;
  tree ident = get_identifier (attrname);

  if (value)
    value = tree_cons (NULL_TREE, value, NULL_TREE);

  attrib = tree_cons (ident, value, NULL_TREE);
  DECL_ATTRIBUTES (decl) = merge_attributes (DECL_ATTRIBUTES (decl), attrib);
}

bool d_attribute_p (const char* name)
{
  static StringTable* table;

  if(!table)
    {
      size_t n = 0;
      for (const attribute_spec *p = d_attribute_table; p->name; p++)
        n++;
      
      if(n == 0)
        return false;

      table = new StringTable();
      table->init(n);
    
      for (const attribute_spec *p = d_attribute_table; p->name; p++)
        table->insert(p->name, strlen(p->name));
    }

  return table->lookup(name, strlen(name)) != NULL;
}

// Return chain of all GCC attributes found in list IN_ATTRS.

tree
build_attributes (Expressions *in_attrs)
{
  if (!in_attrs)
    return NULL_TREE;
  
  expandTuples(in_attrs);
  
  ListMaker out_attrs;

  for (size_t i = 0; i < in_attrs->dim; i++)
    {
      Expression *attr = (*in_attrs)[i]->ctfeInterpret();
      Dsymbol *sym = attr->type->toDsymbol (0);

      if (!sym)
	continue;

      Dsymbol *mod = (Dsymbol*) sym->getModule();  
      if (!(strcmp(mod->toChars(), "attribute") == 0
          && mod->parent 
          && strcmp(mod->parent->toChars(), "gcc") == 0
          && !mod->parent->parent))
        continue;

      gcc_assert(attr->op == TOKstructliteral);
      Expressions *elem = ((StructLiteralExp*) attr)->elements;

      if ((*elem)[0]->op == TOKnull)
	{
	  error ("expected string attribute, not null");
	  return error_mark_node;
	}

      gcc_assert((*elem)[0]->op == TOKstring);
      StringExp *nameExp = (StringExp*) (*elem)[0];
      gcc_assert(nameExp->sz == 1);
      const char* name = (const char*) nameExp->string;

      if (!d_attribute_p (name))
      {
        error ("unknown attribute %s", name);
        return error_mark_node;
      }

      ListMaker args;
      
      for (size_t j = 1; j < elem->dim; j++)
        {
	  Expression *ae = (*elem)[j];
	  tree aet;
	  if (ae->op == TOKstring && ((StringExp *) ae)->sz == 1)
	    {
	      StringExp *s = (StringExp *) ae;
	      aet = build_string (s->len, (const char *) s->string);
	    }
	  else
	    aet = ae->toElem (&gen);
	  args.cons (aet);
        }

      out_attrs.cons (get_identifier (name), args.head);
    }

  return out_attrs.head;
}

// Return qualified type variant of TYPE determined by modifier value MOD.

tree
insert_type_modifiers (tree type, unsigned mod)
{
  int quals = 0;
  gcc_assert (type);

  switch (mod)
    {
    case 0:
      break;

    case MODconst:
    case MODwild:
    case MODimmutable:
      quals |= TYPE_QUAL_CONST;
      break;

    case MODshared:
      quals |= TYPE_QUAL_VOLATILE;
      break;

    case MODshared | MODwild:
    case MODshared | MODconst:
      quals |= TYPE_QUAL_CONST;
      quals |= TYPE_QUAL_VOLATILE;
      break;

    default:
      gcc_unreachable();
    }

  return build_qualified_type (type, quals);
}

// Build INTEGER_CST of type TYPE with the value VALUE.

tree
build_integer_cst (dinteger_t value, tree type)
{
  // The type is error_mark_node, we can't do anything.
  if (error_mark_p (type))
    return type;

  return build_int_cst_type (type, value);
}

// Build REAL_CST of type TARGET_TYPE with the value VALUE.

tree
build_float_cst (const real_t& value, Type *target_type)
{
  real_t new_value;
  TypeBasic *tb = target_type->isTypeBasic();

  gcc_assert (tb != NULL);

  tree type_node = tb->toCtype();
  real_convert (&new_value.rv(), TYPE_MODE (type_node), &value.rv());

  if (new_value > value)
    {
      // value grew as a result of the conversion. %% precision bug ??
      // For now just revert back to original.
      new_value = value;
    }

  return build_real (type_node, new_value.rv());
}

// Convert LOW / HIGH pair into dinteger_t type.

dinteger_t
cst_to_hwi (double_int cst)
{
  if (cst.high == 0 || (cst.high == -1 && (HOST_WIDE_INT) cst.low < 0))
    return cst.low;
  else if (cst.low == 0 && cst.high == 1)
    return (~(dinteger_t) 0);

  gcc_unreachable();
}

// Return host integer value for INT_CST T.

dinteger_t
tree_to_hwi (tree t)
{
  if (host_integerp (t, 0) || host_integerp (t, 1))
    return tree_low_cst (t, 1);

  return cst_to_hwi (TREE_INT_CST (t));
}

// Returns the .length component from the D dynamic array EXP.

tree
d_array_length (tree exp)
{
  // backend will ICE otherwise
  if (error_mark_p (exp))
    return exp;

  // Get the backend type for the array and pick out the array
  // length field (assumed to be the first field.)
  tree len_field = TYPE_FIELDS (TREE_TYPE (exp));
  return component_ref (exp, len_field);
}

// Returns the .ptr component from the D dynamic array EXP.

tree
d_array_ptr (tree exp)
{
  // backend will ICE otherwise
  if (error_mark_p (exp))
    return exp;

  // Get the backend type for the array and pick out the array
  // data pointer field (assumed to be the second field.)
  tree ptr_field = TREE_CHAIN (TYPE_FIELDS (TREE_TYPE (exp)));
  return component_ref (exp, ptr_field);
}

// Returns a constructor for D dynamic array type TYPE of .length LEN
// and .ptr pointing to DATA.

tree
d_array_value (tree type, tree len, tree data)
{
  // %% assert type is a darray
  tree len_field, ptr_field;
  CtorEltMaker ce;

  len_field = TYPE_FIELDS (type);
  ptr_field = TREE_CHAIN (len_field);

  len = convert (TREE_TYPE (len_field), len);
  data = convert (TREE_TYPE (ptr_field), data);

  ce.cons (len_field, len);
  ce.cons (ptr_field, data);

  tree ctor = build_constructor (type, ce.head);
  // TREE_STATIC and TREE_CONSTANT can be set by caller if needed
  TREE_STATIC (ctor) = 0;
  TREE_CONSTANT (ctor) = 0;

  return ctor;
}

// Builds a D string value from the C string STR.

tree
d_array_string (const char *str)
{
  unsigned len = strlen (str);
  // Assumes str is null-terminated.
  tree str_tree = build_string (len + 1, str);

  TREE_TYPE (str_tree) = d_array_type (Type::tchar, len);

  return d_array_value (Type::tchar->arrayOf()->toCtype(),
			size_int (len), build_address (str_tree));
}

// Returns value representing the array length of expression EXP.
// TYPE could be a dynamic or static array.

tree
get_array_length (tree exp, Type *type)
{
  Type *tb = type->toBasetype();

  switch (tb->ty)
    {
    case Tsarray:
      return size_int (((TypeSArray *) tb)->dim->toUInteger());

    case Tarray:
      return d_array_length (exp);

    default:
      ::error ("can't determine the length of a %s", type->toChars());
      return error_mark_node;
    }
}

// Return TRUE if binary expression EXP is an unhandled array operation,
// in which case we error that it is not implemented.

bool
unhandled_arrayop_p (BinExp *exp)
{
  TY ty1 = exp->e1->type->toBasetype()->ty;
  TY ty2 = exp->e2->type->toBasetype()->ty;

  if ((ty1 == Tarray || ty1 == Tsarray
       || ty2 == Tarray || ty2 == Tsarray))
    {
      exp->error ("Array operation %s not implemented", exp->toChars());
      return true;
    }
  return false;
}

// Returns the .funcptr component from the D delegate EXP.

tree
delegate_method (tree exp)
{
  // Get the backend type for the array and pick out the array length
  // field (assumed to be the second field.)
  tree method_field = TREE_CHAIN (TYPE_FIELDS (TREE_TYPE (exp)));
  return component_ref (exp, method_field);
}

// Returns the .object component from the delegate EXP.

tree
delegate_object (tree exp)
{
  // Get the backend type for the array and pick out the array data
  // pointer field (assumed to be the first field.)
  tree obj_field = TYPE_FIELDS (TREE_TYPE (exp));
  return component_ref (exp, obj_field);
}

// Build a delegate literal of type TYPE whose pointer function is
// METHOD, and hidden object is OBJECT.  

tree
build_delegate_cst (tree method, tree object, Type *type)
{
  Type *base_type = type->toBasetype();

  // Called from DotVarExp.  These are just used to make function calls
  // and not to make Tdelegate variables.  Clearing the type makes sure of this.
  if (base_type->ty == Tfunction)
    base_type = NULL;
  else
    gcc_assert (base_type->ty == Tdelegate);

  tree ctype = base_type ? base_type->toCtype() : NULL_TREE;
  tree ctor = make_node (CONSTRUCTOR);
  tree obj_field = NULL_TREE;
  tree func_field = NULL_TREE;
  CtorEltMaker ce;

  if (ctype)
    {
      TREE_TYPE (ctor) = ctype;
      obj_field = TYPE_FIELDS (ctype);
      func_field = TREE_CHAIN (obj_field);
    }
  ce.cons (obj_field, object);
  ce.cons (func_field, method);

  CONSTRUCTOR_ELTS (ctor) = ce.head;
  return ctor;
}

// Builds a temporary tree to store the CALLEE and OBJECT
// of a method call expression of type TYPE.

tree
build_method_call (tree callee, tree object, Type *type)
{
  tree t = build_delegate_cst (callee, object, type);
  D_METHOD_CALL_EXPR (t) = 1;
  return t;
}

// Extract callee and object from T and return in to CALLEE and OBJECT.

void
extract_from_method_call (tree t, tree& callee, tree& object)
{
  gcc_assert (D_METHOD_CALL_EXPR (t));
  vec<constructor_elt, va_gc>* elts = CONSTRUCTOR_ELTS (t);
  object = (*elts)[0].value;
  callee = (*elts)[1].value;
}

// Return correct callee for method FUNC, which is dereferenced from
// the 'this' pointer OBJEXP.  TYPE is the return type for the method.

tree
get_object_method (Expression *objexp, FuncDeclaration *func, Type *type)
{
  Type *objtype = objexp->type->toBasetype();

  if (func->isThis())
    {
      bool is_dottype = false;
      tree this_expr;

      Expression *ex = objexp;
      while (1)
	{
	  switch (ex->op)
	    {
	      case TOKsuper:          // super.member() calls directly
	      case TOKdottype:        // type.member() calls directly
		is_dottype = true;
		break;

	      case TOKcast:
		ex = ((CastExp *)ex)->e1;
		continue;

	      default:
		break;
	    }
	  break;
	}
      this_expr = objexp->toElem (current_irs);

      // Calls to super are static (func is the super's method)
      // Structs don't have vtables.
      // Final and non-virtual methods can be called directly.
      // DotTypeExp means non-virtual

      if (objexp->op == TOKsuper
	  || objtype->ty == Tstruct || objtype->ty == Tpointer
	  || func->isFinal() || !func->isVirtual() || is_dottype)
	{
	  if (objtype->ty == Tstruct)
	    this_expr = build_address (this_expr);

	  return build_method_call (build_address (func->toSymbol()->Stree),
				    this_expr, type);
	}
      else
	{
	  // Interface methods are also in the class's vtable, so we don't
	  // need to convert from a class pointer to an interface pointer.
	  this_expr = maybe_make_temp (this_expr);
	  tree vtbl_ref = build_deref (this_expr);
	  // The vtable is the first field.
	  tree field = TYPE_FIELDS (TREE_TYPE (vtbl_ref));
	  tree fntype = TREE_TYPE (func->toSymbol()->Stree);

	  vtbl_ref = component_ref (vtbl_ref, field);
	  vtbl_ref = build_offset (vtbl_ref, size_int (PTRSIZE * func->vtblIndex));
	  vtbl_ref = indirect_ref (build_pointer_type (fntype), vtbl_ref);

	  return build_method_call (vtbl_ref, this_expr, type);
	}
    }
  else
    {
      // Static method; ignore the object instance
      return build_address (func->toSymbol()->Stree);
    }
}


// Builds a record type from field types T1 and T2.  TYPE is the D frontend
// type we are building. N1 and N2 are the names of the two fields.

tree
build_two_field_type (tree t1, tree t2, Type *type, const char *n1, const char *n2)
{
  tree rec_type = make_node (RECORD_TYPE);
  tree f0 = build_decl (BUILTINS_LOCATION, FIELD_DECL, get_identifier (n1), t1);
  tree f1 = build_decl (BUILTINS_LOCATION, FIELD_DECL, get_identifier (n2), t2);
  DECL_CONTEXT (f0) = rec_type;
  DECL_CONTEXT (f1) = rec_type;
  TYPE_FIELDS (rec_type) = chainon (f0, f1);
  layout_type (rec_type);
  if (type)
    {
      /* This is needed so that maybeExpandSpecialCall knows to
	 split dynamic array varargs. */
      TYPE_LANG_SPECIFIC (rec_type) = build_d_type_lang_specific (type);

      /* ObjectFile::declareType will try to declare it as top-level type
	 which can break debugging info for element types. */
      tree stub_decl = build_decl (BUILTINS_LOCATION, TYPE_DECL,
				   get_identifier (type->toChars()), rec_type);
      TYPE_STUB_DECL (rec_type) = stub_decl;
      TYPE_NAME (rec_type) = stub_decl;
      DECL_ARTIFICIAL (stub_decl) = 1;
      rest_of_decl_compilation (stub_decl, 0, 0);
    }
  return rec_type;
}

// Create a SAVE_EXPR if T might have unwanted side effects if referenced
// more than once in an expression.

tree
maybe_make_temp (tree t)
{
  if (d_has_side_effects (t))
    {
      if (TREE_CODE (t) == CALL_EXPR
	  || TREE_CODE (TREE_TYPE (t)) != ARRAY_TYPE)
	return save_expr (t);
      else
	return stabilize_reference (t);
    }

  return t;
}

// Return TRUE if T can not be evaluated multiple times (i.e., in a loop body)
// without unwanted side effects.

bool
d_has_side_effects (tree expr)
{
  tree t = STRIP_NOPS (expr);

  // SAVE_EXPR is safe to reference more than once, but not to
  // expand in a loop.
  if (TREE_CODE (t) == SAVE_EXPR)
    return false;

  if (DECL_P (t)
      || CONSTANT_CLASS_P (t))
    return false;

  if (INDIRECT_REF_P (t)
      || TREE_CODE (t) == ADDR_EXPR
      || TREE_CODE (t) == COMPONENT_REF)
    return d_has_side_effects (TREE_OPERAND (t, 0));

  return TREE_SIDE_EFFECTS (t);
}

// Evaluates expression E as an Lvalue.

tree
IRState::toElemLvalue (Expression *e)
{
  if (e->op == TOKindex)
    {
      IndexExp *ie = (IndexExp *) e;
      Expression *e1 = ie->e1;
      Expression *e2 = ie->e2;
      Type *type = e->type;
      Type *array_type = e1->type->toBasetype();

      if (array_type->ty == Taarray)
	{
	  Type *key_type = ((TypeAArray *) array_type)->index->toBasetype();
	  AddrOfExpr aoe;
	  tree args[4];
	  tree result;

	  args[0] = build_address (toElemLvalue (e1));
	  args[1] = typeinfoReference (key_type);
	  args[2] = build_integer_cst (array_type->nextOf()->size(), Type::tsize_t->toCtype());
	  args[3] = aoe.set (this, convertTo (e2, key_type));

	  result = aoe.finish (build_libcall (LIBCALL_AAGETX, 4, args,
					      type->pointerTo()->toCtype()));
	  return build1 (INDIRECT_REF, type->toCtype(), result);
	}
    }

  return e->toElem (this);
}

// Returns the address of the expression EXP.

tree
build_address (tree exp)
{
  tree t, ptrtype;
  tree exp_type = TREE_TYPE (exp);
  d_mark_addressable (exp);

  // Gimplify doesn't like &(* (ptr-to-array-type)) with static arrays
  if (TREE_CODE (exp) == INDIRECT_REF)
    {
      t = TREE_OPERAND (exp, 0);
      ptrtype = build_pointer_type (exp_type);
      t = build_nop (ptrtype, t);
    }
  else
    {
      /* Just convert string literals (char[]) to C-style strings (char *), otherwise
	 the latter method (char[]*) causes conversion problems during gimplification. */
      if (TREE_CODE (exp) == STRING_CST)
	ptrtype = build_pointer_type (TREE_TYPE (exp_type));
      /* Special case for va_list. The backends will be expecting a pointer to vatype,
       * but some targets use an array. So fix it.  */
      else if (TYPE_MAIN_VARIANT (exp_type) == TYPE_MAIN_VARIANT (va_list_type_node))
	{
	  if (TREE_CODE (TYPE_MAIN_VARIANT (exp_type)) == ARRAY_TYPE)
	    ptrtype = build_pointer_type (TREE_TYPE (exp_type));
	  else
	    ptrtype = build_pointer_type (exp_type);
	}
      else
	ptrtype = build_pointer_type (exp_type);

      t = build1 (ADDR_EXPR, ptrtype, exp);
    }

  if (TREE_CODE (exp) == FUNCTION_DECL)
    TREE_NO_TRAMPOLINE (t) = 1;

  return t;
}

tree
d_mark_addressable (tree exp)
{
  switch (TREE_CODE (exp))
    {
    case ADDR_EXPR:
    case COMPONENT_REF:
      /* If D had bit fields, we would need to handle that here */
    case ARRAY_REF:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
      d_mark_addressable (TREE_OPERAND (exp, 0));
      break;

      /* %% C++ prevents {& this} .... */
      /* %% TARGET_EXPR ... */
    case TRUTH_ANDIF_EXPR:
    case TRUTH_ORIF_EXPR:
    case COMPOUND_EXPR:
      d_mark_addressable (TREE_OPERAND (exp, 1));
      break;

    case COND_EXPR:
      d_mark_addressable (TREE_OPERAND (exp, 1));
      d_mark_addressable (TREE_OPERAND (exp, 2));
      break;

    case CONSTRUCTOR:
      TREE_ADDRESSABLE (exp) = 1;
      break;

    case INDIRECT_REF:
      /* %% this was in Java, not sure for D */
      /* We sometimes add a cast *(TYPE *)&FOO to handle type and mode
	 incompatibility problems.  Handle this case by marking FOO.  */
      if (TREE_CODE (TREE_OPERAND (exp, 0)) == NOP_EXPR
	  && TREE_CODE (TREE_OPERAND (TREE_OPERAND (exp, 0), 0)) == ADDR_EXPR)
	{
	  d_mark_addressable (TREE_OPERAND (TREE_OPERAND (exp, 0), 0));
	  break;
	}
      if (TREE_CODE (TREE_OPERAND (exp, 0)) == ADDR_EXPR)
	{
	  d_mark_addressable (TREE_OPERAND (exp, 0));
	  break;
	}
      break;

    case VAR_DECL:
    case CONST_DECL:
    case PARM_DECL:
    case RESULT_DECL:
    case FUNCTION_DECL:
      TREE_USED (exp) = 1;
      TREE_ADDRESSABLE (exp) = 1;

      /* drops through */
    default:
      break;
    }

  return exp;
}

/* Mark EXP as "used" in the program for the benefit of
   -Wunused warning purposes.  */

tree
d_mark_used (tree exp)
{
  switch (TREE_CODE (exp))
    {
    case VAR_DECL:
    case PARM_DECL:
      TREE_USED (exp) = 1;
      break;

    case ARRAY_REF:
    case COMPONENT_REF:
    case MODIFY_EXPR:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
    case NOP_EXPR:
    case CONVERT_EXPR:
    case ADDR_EXPR:
      d_mark_used (TREE_OPERAND (exp, 0));
      break;

    case COMPOUND_EXPR:
      d_mark_used (TREE_OPERAND (exp, 0));
      d_mark_used (TREE_OPERAND (exp, 1));
      break;

    default:
      break;
    }
  return exp;
}

/* Mark EXP as read, not just set, for set but not used -Wunused
   warning purposes.  */

tree
d_mark_read (tree exp)
{
  switch (TREE_CODE (exp))
    {
    case VAR_DECL:
    case PARM_DECL:
      TREE_USED (exp) = 1;
      DECL_READ_P (exp) = 1;
      break;

    case ARRAY_REF:
    case COMPONENT_REF:
    case MODIFY_EXPR:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
    case NOP_EXPR:
    case CONVERT_EXPR:
    case ADDR_EXPR:
      d_mark_read (TREE_OPERAND (exp, 0));
      break;

    case COMPOUND_EXPR:
      d_mark_read (TREE_OPERAND (exp, 1));
      break;

    default:
      break;
    }
  return exp;
}

// Cast EXP (which should be a pointer) to TYPE * and then indirect.  The
// back-end requires this cast in many cases.

tree
indirect_ref (tree type, tree exp)
{
  if (TREE_CODE (TREE_TYPE (exp)) == REFERENCE_TYPE)
    return build1 (INDIRECT_REF, type, exp);

  return build1 (INDIRECT_REF, type,
		 build_nop (build_pointer_type (type), exp));
}

// Returns indirect reference of EXP, which must be a pointer type.

tree
build_deref (tree exp)
{
  tree type = TREE_TYPE (exp);
  gcc_assert (POINTER_TYPE_P (type));

  if (TREE_CODE (exp) == ADDR_EXPR)
    return TREE_OPERAND (exp, 0);

  return build1 (INDIRECT_REF, TREE_TYPE (type), exp);
}

// Builds pointer offset expression PTR_EXP[IDX_EXP]

tree
IRState::pointerIntSum (Expression *ptr_exp, Expression *idx_exp)
{
  return pointerIntSum (ptr_exp->toElem (this), idx_exp->toElem (this));
}

tree
IRState::pointerIntSum (tree ptr_node, tree idx_exp)
{
  tree result_type_node = TREE_TYPE (ptr_node);
  tree elem_type_node = TREE_TYPE (result_type_node);
  tree intop = idx_exp;
  tree size_exp;

  tree prod_result_type;
  prod_result_type = sizetype;

  size_exp = size_in_bytes (elem_type_node); // array element size

  if (integer_zerop (size_exp))
    {
      // Test for void case...
      if (TYPE_MODE (elem_type_node) == TYPE_MODE (void_type_node))
	intop = fold_convert (prod_result_type, intop);
      else
	{
	  // FIXME: should catch this earlier.
	  error ("invalid use of incomplete type %qD", TYPE_NAME (elem_type_node));
	  result_type_node = error_mark_node;
	}
    }
  else if (integer_onep (size_exp))
    {
      // ...or byte case -- No need to multiply.
      intop = fold_convert (prod_result_type, intop);
    }
  else
    {
      if (TYPE_PRECISION (TREE_TYPE (intop)) != TYPE_PRECISION (sizetype)
	  || TYPE_UNSIGNED (TREE_TYPE (intop)) != TYPE_UNSIGNED (sizetype))
	{
	  tree type = lang_hooks.types.type_for_size (TYPE_PRECISION (sizetype),
						      TYPE_UNSIGNED (sizetype));
	  intop = convertTo (type, intop);
	}
      intop = fold_convert (prod_result_type,
			    fold_build2 (MULT_EXPR, TREE_TYPE (size_exp), // the type here may be wrong %%
					 intop, convertTo (TREE_TYPE (intop), size_exp)));
    }

  // backend will ICE otherwise
  if (error_mark_p (result_type_node))
    return result_type_node;

  if (integer_zerop (intop))
    return ptr_node;

  return build2 (POINTER_PLUS_EXPR, result_type_node, ptr_node, intop);
}

// Builds pointer offset expression *(PTR OP IDX)
// OP could be a plus or minus expression.

tree
build_offset_op (enum tree_code op, tree ptr, tree idx)
{
  gcc_assert (op == MINUS_EXPR || op == PLUS_EXPR);

  if (op == MINUS_EXPR)
    idx = fold_build1 (NEGATE_EXPR, sizetype, idx);

  return build2 (POINTER_PLUS_EXPR, TREE_TYPE (ptr), ptr,
		 fold_convert (sizetype, idx));
}

tree
build_offset (tree ptr_node, tree byte_offset)
{
  tree ofs = fold_convert (Type::tsize_t->toCtype(), byte_offset);
  return fold_build2 (POINTER_PLUS_EXPR, TREE_TYPE (ptr_node), ptr_node, ofs);
}


// Implicitly converts void* T to byte* as D allows { void[] a; &a[3]; }

tree
void_okay_p (tree t)
{
  tree type = TREE_TYPE (t);
  tree totype = Type::tuns8->pointerTo()->toCtype();

  if (VOID_TYPE_P (TREE_TYPE (type)))
    return convert (totype, t);

  return t;
}

// Build an expression of code CODE, data type TYPE, and operands ARG0
// and ARG1. Perform relevant conversions needs for correct code operations.

tree
IRState::buildOp (tree_code code, tree type, tree arg0, tree arg1)
{
  tree t0 = TREE_TYPE (arg0);
  tree t1 = TREE_TYPE (arg1);

  bool unsignedp = TYPE_UNSIGNED (t0) || TYPE_UNSIGNED (t1);

  tree t = NULL_TREE;

  // Deal with float mod expressions immediately.
  if (code == FLOAT_MOD_EXPR)
    return floatMod (TREE_TYPE (arg0), arg0, arg1);

  if (POINTER_TYPE_P (t0) && INTEGRAL_TYPE_P (t1))
    return build_nop (type, build_offset_op (code, arg0, arg1));

  if (INTEGRAL_TYPE_P (t0) && POINTER_TYPE_P (t1))
    return build_nop (type, build_offset_op (code, arg1, arg0));

  if (POINTER_TYPE_P (t0) && POINTER_TYPE_P (t1))
    {
      // Need to convert pointers to integers because tree-vrp asserts
      // against (ptr MINUS ptr).
      tree ptrtype = lang_hooks.types.type_for_mode (ptr_mode, TYPE_UNSIGNED (type));
      arg0 = convertTo (ptrtype, arg0);
      arg1 = convertTo (ptrtype, arg1);

      t = build2 (code, ptrtype, arg0, arg1);
    }
  else if (INTEGRAL_TYPE_P (type) && (TYPE_UNSIGNED (type) != unsignedp))
    {
      t = build2 (code, unsignedp ? d_unsigned_type (type) : d_signed_type (type),
		  arg0, arg1);
    }
  else
    {
      // Front-end does not do this conversion and GCC does not
      // always do it right.
      if (COMPLEX_FLOAT_TYPE_P (t0) && !COMPLEX_FLOAT_TYPE_P (t1))
	arg1 = convertTo (t0, arg1);
      else if (COMPLEX_FLOAT_TYPE_P (t1) && !COMPLEX_FLOAT_TYPE_P (t0))
	arg0 = convertTo (t1, arg0);

      t = build2 (code, type, arg0, arg1);
    }

  return convertTo (type, t);
}

// Build an assignment expression of code CODE, data type TYPE, and
// operands E1 and E2.

tree
IRState::buildAssignOp (tree_code code, Type *type, Expression *e1, Expression *e2)
{
  // Skip casts for lhs assignment.
  Expression *e1b = e1;
  while (e1b->op == TOKcast)
    {
      CastExp *ce = (CastExp *) e1b;
      gcc_assert (d_types_compatible (ce->type, ce->to));
      e1b = ce->e1;
    }

  // Prevent multiple evaluations of LHS
  tree lhs = toElemLvalue (e1b);
  lhs = stabilize_reference (lhs);

  tree rhs = buildOp (code, e1->type->toCtype(),
		      convertTo (lhs, e1b->type, e1->type), e2->toElem (this));

  tree expr = modify_expr (lhs, convertForAssignment (rhs, e1->type, e1b->type));

  return convertTo (expr, e1b->type, type);
}


// Builds an array bounds checking condition, returning INDEX if true,
// else throws a RangeError exception.

tree
IRState::checkedIndex (Loc loc, tree index, tree upper_bound, bool inclusive)
{
  if (!arrayBoundsCheck())
    return index;

  return build3 (COND_EXPR, TREE_TYPE (index),
		 boundsCond (index, upper_bound, inclusive),
		 index, assertCall (loc, LIBCALL_ARRAY_BOUNDS));
}

// Builds the condition [INDEX < UPPER_BOUND] and optionally [INDEX >= 0]
// if INDEX is a signed type.  For use in array bound checking routines.
// If INCLUSIVE, we allow equality to return true also.
// INDEX must be wrapped in a SAVE_EXPR to prevent multiple evaluation...

tree
IRState::boundsCond (tree index, tree upper_bound, bool inclusive)
{
  tree bound_check;

  bound_check = build2 (inclusive ? LE_EXPR : LT_EXPR, boolean_type_node,
			convertTo (d_unsigned_type (TREE_TYPE (index)), index),
			upper_bound);

  if (!TYPE_UNSIGNED (TREE_TYPE (index)))
    {
      bound_check = build2 (TRUTH_ANDIF_EXPR, boolean_type_node, bound_check,
			    // %% conversions
			    build2 (GE_EXPR, boolean_type_node, index, integer_zero_node));
    }
  return bound_check;
}

// Returns TRUE if array bounds checking code generation is turned on.

int
IRState::arrayBoundsCheck (void)
{
  int result = global.params.useArrayBounds;

  if (result == 1)
    {
      // For D2 safe functions only
      result = 0;
      if (func && func->type->ty == Tfunction)
	{
	  TypeFunction *tf = (TypeFunction *)func->type;
	  if (tf->trust == TRUSTsafe)
	    result = 1;
	}
    }

  return result;
}

// Builds an array index expression from AE.  ASC may build a
// BIND_EXPR if temporaries were created for bounds checking.

tree
IRState::arrayElemRef (IndexExp *ae, ArrayScope *asc)
{
  Expression *e1 = ae->e1;
  Expression *e2 = ae->e2;

  Type *base_type = e1->type->toBasetype();
  TY base_type_ty = base_type->ty;
  // expression that holds the array data.
  tree array_expr = e1->toElem (this);
  // expression that indexes the array data
  tree subscript_expr = e2->toElem (this);
  // base pointer to the elements
  tree ptr_exp;
  // reference the the element
  tree elem_ref;

  switch (base_type_ty)
    {
    case Tarray:
    case Tsarray:
      array_expr = asc->setArrayExp (array_expr, e1->type);

      // If it's a static array and the index is constant,
      // the front end has already checked the bounds.
      if (arrayBoundsCheck()
	  && !(base_type_ty == Tsarray && e2->isConst()))
	{
	  tree array_len_expr;
	  // implement bounds check as a conditional expression:
	  // array [inbounds(index) ? index : { throw ArrayBoundsError }]

	  // First, set up the index expression to only be evaluated once.
	  tree index_expr = maybe_make_temp (subscript_expr);

	  if (base_type_ty == Tarray)
	    {
	      array_expr = maybe_make_temp (array_expr);
	      array_len_expr = d_array_length (array_expr);
	    }
	  else
	    array_len_expr = ((TypeSArray *) base_type)->dim->toElem (this);

	  subscript_expr = checkedIndex (ae->loc, index_expr,
					 array_len_expr, false);
	}

      if (base_type_ty == Tarray)
	ptr_exp = d_array_ptr (array_expr);
      else
	ptr_exp = build_address (array_expr);

      // This conversion is required for static arrays and is just-to-be-safe
      // for dynamic arrays
      ptr_exp = convert (base_type->nextOf()->pointerTo()->toCtype(), ptr_exp);
      break;

    case Tpointer:
      // Ignores array scope.
      ptr_exp = array_expr;
      break;

    default:
      gcc_unreachable();
    }

  ptr_exp = void_okay_p (ptr_exp);
  subscript_expr = asc->finish (subscript_expr);
  elem_ref = indirect_ref (TREE_TYPE (TREE_TYPE (ptr_exp)),
			   pointerIntSum (ptr_exp, subscript_expr));

  return elem_ref;
}


void
IRState::doArraySet (tree in_ptr, tree in_value, tree in_count)
{
  startBindings();

  tree count = localVar (Type::tsize_t);
  DECL_INITIAL (count) = in_count;
  expandDecl (count);

  tree ptr = localVar (TREE_TYPE (in_ptr));
  DECL_INITIAL (ptr) = in_ptr;
  expandDecl (ptr);

  tree ptr_type = TREE_TYPE (ptr);
  tree count_type = TREE_TYPE (count);

  tree value = NULL_TREE;

  if (!d_has_side_effects (in_value))
    value = in_value;
  else
    {
      value = localVar (TREE_TYPE (in_value));
      DECL_INITIAL (value) = in_value;
      expandDecl (value);
    }

  startLoop (NULL);
  continueHere();
  exitIfFalse (build2 (NE_EXPR, boolean_type_node,
		       convertTo (TREE_TYPE (count), integer_zero_node), count));

  addExp (vmodify_expr (build_deref (ptr), value));
  addExp (vmodify_expr (ptr, build_offset (ptr, TYPE_SIZE_UNIT (TREE_TYPE (ptr_type)))));
  addExp (vmodify_expr (count, build2 (MINUS_EXPR, count_type, count,
				       convertTo (count_type, integer_one_node))));

  endLoop();
  endBindings();
}

// Create a tree node to set multiple elements to a single value
tree
IRState::arraySetExpr (tree ptr, tree value, tree count)
{
  pushStatementList();
  doArraySet (ptr, value, count);
  return popStatementList();
}

// Builds a BIND_EXPR around BODY for the variables VAR_CHAIN.

tree
bind_expr (tree var_chain, tree body)
{
  // TODO: only handles one var
  gcc_assert (TREE_CHAIN (var_chain) == NULL_TREE);

  if (DECL_INITIAL (var_chain))
    {
      tree ini = build_vinit (var_chain, DECL_INITIAL (var_chain));
      DECL_INITIAL (var_chain) = NULL_TREE;
      body = compound_expr (ini, body);
    }

  return save_expr (build3 (BIND_EXPR, TREE_TYPE (body), var_chain, body, NULL_TREE));
}

// Like compound_expr, but ARG0 or ARG1 might be NULL_TREE.

tree
maybe_compound_expr (tree arg0, tree arg1)
{
  if (arg0 == NULL_TREE)
    return arg1;
  else if (arg1 == NULL_TREE)
    return arg0;
  else
    return compound_expr (arg0, arg1);
}

// Like vcompound_expr, but ARG0 or ARG1 might be NULL_TREE.

tree
maybe_vcompound_expr (tree arg0, tree arg1)
{
  if (arg0 == NULL_TREE)
    return arg1;
  else if (arg1 == NULL_TREE)
    return arg0;
  else
    return vcompound_expr (arg0, arg1);
}

// Returns TRUE if T is an ERROR_MARK node.

bool
error_mark_p (tree t)
{
  return (t == error_mark_node
	  || (t && TREE_TYPE (t) == error_mark_node)
	  || (t && TREE_CODE (t) == NOP_EXPR
	      && TREE_OPERAND (t, 0) == error_mark_node));
}

// Returns the TypeFunction class for Type T.
// Assumes T is already ->toBasetype()

TypeFunction *
get_function_type (Type *t)
{
  TypeFunction *tf = NULL;
  if (t->ty == Tpointer)
    t = t->nextOf()->toBasetype();
  if (t->ty == Tfunction)
    tf = (TypeFunction *) t;
  else if (t->ty == Tdelegate)
    tf = (TypeFunction *) ((TypeDelegate *) t)->next;
  return tf;
}

// Returns TRUE if CALLEE is a plain nested function outside the scope of CALLER.
// In which case, CALLEE is being called through an alias that was passed to CALLER.

bool
call_by_alias_p (FuncDeclaration *caller, FuncDeclaration *callee)
{
  if (!callee->isNested())
    return false;

  Dsymbol *dsym = callee;

  while (dsym)
    {
      if (dsym->isTemplateInstance())
	return false;
      else if (dsym->isFuncDeclaration() == caller)
	return false;
      dsym = dsym->toParent();
    }

  return true;
}

// Entry point for call routines.  Extracts the callee, object,
// and function type from expression EXPR, passing down ARGUMENTS.

tree
IRState::call (Expression *expr, Expressions *arguments)
{
  // Calls to delegates can sometimes look like this:
  if (expr->op == TOKcomma)
    {
      CommaExp *ce = (CommaExp *) expr;
      expr = ce->e2;

      VarExp *ve;
      gcc_assert (ce->e2->op == TOKvar);
      ve = (VarExp *) ce->e2;
      gcc_assert (ve->var->isFuncDeclaration() && !ve->var->needThis());
    }

  Type *t = expr->type->toBasetype();
  TypeFunction *tf = NULL;
  tree callee = expr->toElem (this);
  tree object = NULL_TREE;

  if (D_METHOD_CALL_EXPR (callee))
    {
      /* This could be a delegate expression (TY == Tdelegate), but not
	 actually a delegate variable. */
      // %% Is this ever not a DotVarExp ?
      if (expr->op == TOKdotvar)
	{
	  /* This gets the true function type, the latter way can sometimes
	     be incorrect. Example: ref functions in D2. */
	  tf = get_function_type (((DotVarExp *)expr)->var->type);
	}
      else
	tf = get_function_type (t);

      extract_from_method_call (callee, callee, object);
    }
  else if (t->ty == Tdelegate)
    {
      tf = (TypeFunction *) ((TypeDelegate *) t)->next;
      callee = maybe_make_temp (callee);
      object = delegate_object (callee);
      callee = delegate_method (callee);
    }
  else if (expr->op == TOKvar)
    {
      FuncDeclaration *fd = ((VarExp *) expr)->var->isFuncDeclaration();
      gcc_assert (fd);
      tf = (TypeFunction *) fd->type;
      if (fd->isNested())
	{
	  if (call_by_alias_p (func, fd))
	    {
	      // Re-evaluate symbol storage treating 'fd' as public.
	      object_file->setupSymbolStorage (fd, callee, true);
	    }
	  object = getFrameForFunction (fd);
	}
      else if (fd->needThis())
	{
	  expr->error ("need 'this' to access member %s", fd->toChars());
	  object = d_null_pointer; // continue processing...
	}
    }
  else
    {
      tf = get_function_type (t);
    }
  return call (tf, callee, object, arguments);
}

// Like above, but is assumed to be a direct call to FUNC_DECL.
// ARGS are the arguments passed.

tree
IRState::call (FuncDeclaration *func_decl, Expressions *args)
{
  // Otherwise need to copy code from above
  gcc_assert (!func_decl->isNested());

  return call (get_function_type (func_decl->type),
	       func_decl->toSymbol()->Stree, NULL_TREE, args);
}

// Like above, but FUNC_DECL is a nested function, method, delegate or lambda.
// OBJECT is the 'this' reference passed and ARGS are the arguments passed.

tree
IRState::call (FuncDeclaration *func_decl, tree object, Expressions *args)
{
  return call (get_function_type (func_decl->type),
	       build_address (func_decl->toSymbol()->Stree), object, args);
}

// Builds a CALL_EXPR of type FUNC_TYPE to CALLABLE. OBJECT holds the 'this' pointer,
// ARGUMENTS are evaluated in left to right order, saved and promoted before passing.

tree
IRState::call (TypeFunction *func_type, tree callable, tree object, Expressions *arguments)
{
  tree func_type_node = TREE_TYPE (callable);
  tree actual_callee = callable;
  tree saved_args = NULL_TREE;

  ListMaker actual_arg_list;

  if (POINTER_TYPE_P (func_type_node))
    func_type_node = TREE_TYPE (func_type_node);
  else
    actual_callee = build_address (callable);

  gcc_assert (function_type_p (func_type_node));
  gcc_assert (func_type != NULL);
  gcc_assert (func_type->ty == Tfunction);

  // Evaluate the callee before calling it.
  if (TREE_SIDE_EFFECTS (actual_callee))
    {
      actual_callee = maybe_make_temp (actual_callee);
      saved_args = actual_callee;
    }

  bool is_d_vararg = func_type->varargs == 1 && func_type->linkage == LINKd;

  if (TREE_CODE (func_type_node) == FUNCTION_TYPE)
    {
      if (object != NULL_TREE)
	gcc_unreachable();
    }
  else if (object == NULL_TREE)
    {
      // Front-end apparently doesn't check this.
      if (TREE_CODE (callable) == FUNCTION_DECL)
	{
	  error ("need 'this' to access member %s", IDENTIFIER_POINTER (DECL_NAME (callable)));
	  return error_mark_node;
	}

      // Probably an internal error
      gcc_unreachable();
    }
  /* If this is a delegate call or a nested function being called as
     a delegate, the object should not be NULL. */
  if (object != NULL_TREE)
    actual_arg_list.cons (object);

  Parameters *formal_args = func_type->parameters; // can be NULL for genCfunc decls
  size_t n_formal_args = formal_args ? (int) Parameter::dim (formal_args) : 0;
  size_t n_actual_args = arguments ? arguments->dim : 0;
  size_t fi = 0;

  // assumes arguments->dim <= formal_args->dim if (!this->varargs)
  for (size_t ai = 0; ai < n_actual_args; ++ai)
    {
      tree actual_arg_tree;
      Expression *actual_arg_exp = (*arguments)[ai];

      if (ai == 0 && is_d_vararg)
	{
	  // The hidden _arguments parameter
	  actual_arg_tree = actual_arg_exp->toElem (this);
	}
      else if (fi < n_formal_args)
	{
	  // Actual arguments for declared formal arguments
	  Parameter *formal_arg = Parameter::getNth (formal_args, fi);
	  actual_arg_tree = convertForArgument (actual_arg_exp, formal_arg);
	  ++fi;
	}
      else
	{
	  if (flag_split_darrays && actual_arg_exp->type->toBasetype()->ty == Tarray)
	    {
	      tree da_exp = maybe_make_temp (actual_arg_exp->toElem (this));
	      actual_arg_list.cons (d_array_length (da_exp));
	      actual_arg_list.cons (d_array_ptr (da_exp));
	      continue;
	    }
	  else
	    {
	      actual_arg_tree = actual_arg_exp->toElem (this);
	      /* Not all targets support passing unpromoted types, so
		 promote anyway. */
	      tree prom_type = lang_hooks.types.type_promotes_to (TREE_TYPE (actual_arg_tree));
	      if (prom_type != TREE_TYPE (actual_arg_tree))
		actual_arg_tree = convert (prom_type, actual_arg_tree);
	    }
	}
      /* Evaluate the argument before passing to the function.
	 Needed for left to right evaluation.  */
      if (func_type->linkage == LINKd && TREE_SIDE_EFFECTS (actual_arg_tree))
	{
	  actual_arg_tree = maybe_make_temp (actual_arg_tree);
	  saved_args = maybe_vcompound_expr (saved_args, actual_arg_tree);
	}

      actual_arg_list.cons (actual_arg_tree);
    }

  tree result = d_build_call (TREE_TYPE (func_type_node), actual_callee, actual_arg_list.head);
  result = maybeExpandSpecialCall (result);

  return maybe_compound_expr (saved_args, result);
}

// Builds a call to AssertError.

tree
IRState::assertCall (Loc loc, LibCall libcall)
{
  tree args[2];

  args[0] = d_array_string (loc.filename ? loc.filename : "");
  args[1] = build_integer_cst (loc.linnum, Type::tuns32->toCtype());

  if (libcall == LIBCALL_ASSERT && this->func->isUnitTestDeclaration())
    libcall = LIBCALL_UNITTEST;

  return build_libcall (libcall, 2, args);
}

// Builds a call to AssertErrorMsg.

tree
IRState::assertCall (Loc loc, Expression *msg)
{
  tree args[3];

  args[0] = msg->toElem (this);
  args[1] = d_array_string (loc.filename ? loc.filename : "");
  args[2] = build_integer_cst (loc.linnum, Type::tuns32->toCtype());

  LibCall libcall = this->func->isUnitTestDeclaration() ?
    LIBCALL_UNITTEST_MSG : LIBCALL_ASSERT_MSG;

  return build_libcall (libcall, 3, args);
}


// Our internal list of library functions.
// Most are extern(C) - for those that are not, correct mangling must be ensured.
// List kept in ascii collating order to allow binary search

static const char *libcall_ids[LIBCALL_count] = {
    /*"_d_invariant",*/ "_D9invariant12_d_invariantFC6ObjectZv",
    "_aApplyRcd1", "_aApplyRcd2", "_aApplyRcw1", "_aApplyRcw2",
    "_aApplyRdc1", "_aApplyRdc2", "_aApplyRdw1", "_aApplyRdw2",
    "_aApplyRwc1", "_aApplyRwc2", "_aApplyRwd1", "_aApplyRwd2",
    "_aApplycd1", "_aApplycd2", "_aApplycw1", "_aApplycw2",
    "_aApplydc1", "_aApplydc2", "_aApplydw1", "_aApplydw2",
    "_aApplywc1", "_aApplywc2", "_aApplywd1", "_aApplywd2",
    "_aaApply", "_aaApply2",
    "_aaDelX", "_aaEqual",
    "_aaGetRvalueX", "_aaGetX",
    "_aaInX", "_aaLen",
    "_adCmp", "_adCmp2",
    "_adDupT", "_adEq", "_adEq2",
    "_adReverse", "_adReverseChar", "_adReverseWchar",
    "_adSort", "_adSortChar", "_adSortWchar",
    "_d_allocmemory", "_d_array_bounds",
    "_d_arrayappendT", "_d_arrayappendcTX",
    "_d_arrayappendcd", "_d_arrayappendwd",
    "_d_arrayassign", "_d_arraycast",
    "_d_arraycatT", "_d_arraycatnT",
    "_d_arraycopy", "_d_arrayctor",
    "_d_arrayliteralTX",
    "_d_arraysetassign", "_d_arraysetctor",
    "_d_arraysetlengthT", "_d_arraysetlengthiT",
    "_d_assert", "_d_assert_msg",
    "_d_assocarrayliteralTX",
    "_d_callfinalizer", "_d_callinterfacefinalizer",
    "_d_criticalenter", "_d_criticalexit",
    "_d_delarray", "_d_delarray_t", "_d_delclass",
    "_d_delinterface", "_d_delmemory",
    "_d_dynamic_cast", "_d_hidden_func", "_d_interface_cast",
    "_d_monitorenter", "_d_monitorexit",
    "_d_newarrayT", "_d_newarrayiT",
    "_d_newarraymTX", "_d_newarraymiTX",
    "_d_newclass", "_d_newitemT", "_d_newitemiT",
    "_d_switch_dstring", "_d_switch_error",
    "_d_switch_string", "_d_switch_ustring",
    "_d_throw", "_d_unittest", "_d_unittest_msg",
};

static FuncDeclaration *libcall_decls[LIBCALL_count];

// Library functions are generated as needed.
// This could probably be changed in the future to be
// more like GCC builtin trees.

FuncDeclaration *
get_libcall (LibCall libcall)
{
  FuncDeclaration *decl = libcall_decls[libcall];

  static Type *aa_type = NULL;
  static Type *dg_type = NULL;
  static Type *dg2_type = NULL;

  if (!decl)
    {
      Types targs;
      Type *treturn = Type::tvoid;
      bool varargs = false;

      // Build generic AA type void*[void*]
      if (aa_type == NULL)
	aa_type = new TypeAArray (Type::tvoidptr, Type::tvoidptr);

      // Build generic delegate type int(void*)
      if (dg_type == NULL)
	{
	  Parameters *fn_parms = new Parameters;
	  fn_parms->push (new Parameter (STCin, Type::tvoidptr, NULL, NULL));
	  Type *fn_type = new TypeFunction (fn_parms, Type::tint32, false, LINKd);
	  dg_type = new TypeDelegate (fn_type);
	}

      // Build generic delegate type int(void*, void*)
      if (dg2_type == NULL)
	{
	  Parameters *fn_parms = new Parameters;
	  fn_parms->push (new Parameter (STCin, Type::tvoidptr, NULL, NULL));
	  fn_parms->push (new Parameter (STCin, Type::tvoidptr, NULL, NULL));
	  Type *fn_type = new TypeFunction (fn_parms, Type::tint32, false, LINKd);
	  dg2_type = new TypeDelegate (fn_type);
	}

      switch (libcall)
	{
	case LIBCALL_ASSERT:
	case LIBCALL_ARRAY_BOUNDS:
	case LIBCALL_SWITCH_ERROR:
	  // need to spec chararray/string because internal code passes string constants
	  targs.push (Type::tchar->arrayOf());
	  targs.push (Type::tuns32);
	  break;

	case LIBCALL_ASSERT_MSG:
	  targs.push (Type::tchar->arrayOf());
	  targs.push (Type::tchar->arrayOf());
	  targs.push (Type::tuns32);
	  break;

	case LIBCALL_UNITTEST:
	  targs.push (Type::tchar->arrayOf());
	  targs.push (Type::tuns32);
	  break;

	case LIBCALL_UNITTEST_MSG:
	  targs.push (Type::tchar->arrayOf());
	  targs.push (Type::tchar->arrayOf());
	  targs.push (Type::tuns32);
	  break;

	case LIBCALL_NEWCLASS:
	  targs.push (ClassDeclaration::classinfo->type->constOf());
	  treturn = build_object_type ();
	  break;

	case LIBCALL_NEWARRAYT:
	case LIBCALL_NEWARRAYIT:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tsize_t);
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_NEWARRAYMTX:
	case LIBCALL_NEWARRAYMITX:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tsize_t);
	  targs.push (Type::tsize_t);
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_NEWITEMT:
	case LIBCALL_NEWITEMIT:
	  targs.push (Type::typeinfo->type->constOf());
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_ALLOCMEMORY:
	  targs.push (Type::tsize_t);
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_DELCLASS:
	case LIBCALL_DELINTERFACE:
	  targs.push (Type::tvoidptr);
	  break;

	case LIBCALL_DELARRAY:
	  targs.push (Type::tvoid->arrayOf()->pointerTo());
	  break;

	case LIBCALL_DELARRAYT:
	  targs.push (Type::tvoid->arrayOf()->pointerTo());
	  targs.push (Type::typeinfo->type->constOf());
	  break;

	case LIBCALL_DELMEMORY:
	  targs.push (Type::tvoidptr->pointerTo());
	  break;

	case LIBCALL_CALLFINALIZER:
	case LIBCALL_CALLINTERFACEFINALIZER:
	  targs.push (Type::tvoidptr);
	  break;

	case LIBCALL_ARRAYSETLENGTHT:
	case LIBCALL_ARRAYSETLENGTHIT:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tsize_t);
	  targs.push (Type::tvoid->arrayOf()->pointerTo());
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_DYNAMIC_CAST:
	case LIBCALL_INTERFACE_CAST:
	  targs.push (build_object_type ());
	  targs.push (ClassDeclaration::classinfo->type);
	  treturn = build_object_type ();
	  break;

	case LIBCALL_ADEQ:
	case LIBCALL_ADEQ2:
	case LIBCALL_ADCMP:
	case LIBCALL_ADCMP2:
	  targs.push (Type::tvoid->arrayOf());
	  targs.push (Type::tvoid->arrayOf());
	  targs.push (Type::typeinfo->type->constOf());
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAEQUAL:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (aa_type);
	  targs.push (aa_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AALEN:
	  targs.push (aa_type);
	  treturn = Type::tsize_t;
	  break;

	case LIBCALL_AAINX:
	  targs.push (aa_type);
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tvoidptr);
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_AAGETX:
	  targs.push (aa_type->pointerTo());
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tsize_t);
	  targs.push (Type::tvoidptr);
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_AAGETRVALUEX:
	  targs.push (aa_type);
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tsize_t);
	  targs.push (Type::tvoidptr);
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_AADELX:
	  targs.push (aa_type);
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tvoidptr);
	  treturn = Type::tbool;
	  break;

	case LIBCALL_ARRAYCAST:
	  targs.push (Type::tsize_t);
	  targs.push (Type::tsize_t);
	  targs.push (Type::tvoid->arrayOf());
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ARRAYCOPY:
	  targs.push (Type::tsize_t);
	  targs.push (Type::tint8->arrayOf());
	  targs.push (Type::tint8->arrayOf());
	  treturn = Type::tint8->arrayOf();
	  break;

	case LIBCALL_ARRAYCATT:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tint8->arrayOf());
	  targs.push (Type::tint8->arrayOf());
	  treturn = Type::tint8->arrayOf();
	  break;

	case LIBCALL_ARRAYCATNT:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tuns32); // Currently 'uint', even if 64-bit
	  varargs = true;
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ARRAYAPPENDT:
	  targs.push (Type::typeinfo->type); //->constOf());
	  targs.push (Type::tint8->arrayOf()->pointerTo());
	  targs.push (Type::tint8->arrayOf());
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ARRAYAPPENDCTX:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tint8->arrayOf()->pointerTo());
	  targs.push (Type::tsize_t);
	  treturn = Type::tint8->arrayOf();
	  break;

	case LIBCALL_ARRAYAPPENDCD:
	  targs.push (Type::tint8->arrayOf()->pointerTo());
	  targs.push (Type::tdchar);
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ARRAYAPPENDWD:
	  targs.push (Type::tint8->arrayOf()->pointerTo());
	  targs.push (Type::tdchar);
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ARRAYASSIGN:
	case LIBCALL_ARRAYCTOR:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tvoid->arrayOf());
	  targs.push (Type::tvoid->arrayOf());
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ARRAYSETASSIGN:
	case LIBCALL_ARRAYSETCTOR:
	  targs.push (Type::tvoidptr);
	  targs.push (Type::tvoidptr);
	  targs.push (Type::tsize_t);
	  targs.push (Type::typeinfo->type->constOf());
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_MONITORENTER:
	case LIBCALL_MONITOREXIT:
	case LIBCALL_THROW:
	case LIBCALL_INVARIANT:
	  targs.push (build_object_type ());
	  break;

	case LIBCALL_CRITICALENTER:
	case LIBCALL_CRITICALEXIT:
	  targs.push (Type::tvoidptr);
	  break;

	case LIBCALL_SWITCH_USTRING:
	  targs.push (Type::twchar->arrayOf()->arrayOf());
	  targs.push (Type::twchar->arrayOf());
	  treturn = Type::tint32;
	  break;

	case LIBCALL_SWITCH_DSTRING:
	  targs.push (Type::tdchar->arrayOf()->arrayOf());
	  targs.push (Type::tdchar->arrayOf());
	  treturn = Type::tint32;
	  break;

	case LIBCALL_SWITCH_STRING:
	  targs.push (Type::tchar->arrayOf()->arrayOf());
	  targs.push (Type::tchar->arrayOf());
	  treturn = Type::tint32;
	  break;
	case LIBCALL_ASSOCARRAYLITERALTX:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tvoid->arrayOf());
	  targs.push (Type::tvoid->arrayOf());
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_ARRAYLITERALTX:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tsize_t);
	  treturn = Type::tvoidptr;
	  break;

	case LIBCALL_ADSORTCHAR:
	case LIBCALL_ADREVERSECHAR:
	  targs.push (Type::tchar->arrayOf());
	  treturn = Type::tchar->arrayOf();
	  break;

	case LIBCALL_ADSORTWCHAR:
	case LIBCALL_ADREVERSEWCHAR:
	  targs.push (Type::twchar->arrayOf());
	  treturn = Type::twchar->arrayOf();
	  break;

	case LIBCALL_ADDUPT:
	  targs.push (Type::typeinfo->type->constOf());
	  targs.push (Type::tvoid->arrayOf());
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ADREVERSE:
	  targs.push (Type::tvoid->arrayOf());
	  targs.push (Type::tsize_t);
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_ADSORT:
	  targs.push (Type::tvoid->arrayOf());
	  targs.push (Type::typeinfo->type->constOf());
	  treturn = Type::tvoid->arrayOf();
	  break;

	case LIBCALL_AAAPPLY:
	  targs.push (aa_type);
	  targs.push (Type::tsize_t);
	  targs.push (dg_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAAPPLY2:
	  targs.push (aa_type);
	  targs.push (Type::tsize_t);
	  targs.push (dg2_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAPPLYCD1:
	case LIBCALL_AAPPLYCW1:
	case LIBCALL_AAPPLYRCD1:
	case LIBCALL_AAPPLYRCW1:
	  targs.push (Type::tchar->arrayOf());
	  targs.push (dg_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAPPLYCD2:
	case LIBCALL_AAPPLYCW2:
	case LIBCALL_AAPPLYRCD2:
	case LIBCALL_AAPPLYRCW2:
	  targs.push (Type::tchar->arrayOf());
	  targs.push (dg2_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAPPLYDC1:
	case LIBCALL_AAPPLYDW1:
	case LIBCALL_AAPPLYRDC1:
	case LIBCALL_AAPPLYRDW1:
	  targs.push (Type::tdchar->arrayOf());
	  targs.push (dg_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAPPLYDC2:
	case LIBCALL_AAPPLYDW2:
	case LIBCALL_AAPPLYRDC2:
	case LIBCALL_AAPPLYRDW2:
	  targs.push (Type::tdchar->arrayOf());
	  targs.push (dg2_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAPPLYWC1:
	case LIBCALL_AAPPLYWD1:
	case LIBCALL_AAPPLYRWC1:
	case LIBCALL_AAPPLYRWD1:
	  targs.push (Type::twchar->arrayOf());
	  targs.push (dg_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_AAPPLYWC2:
	case LIBCALL_AAPPLYWD2:
	case LIBCALL_AAPPLYRWC2:
	case LIBCALL_AAPPLYRWD2:
	  targs.push (Type::twchar->arrayOf());
	  targs.push (dg2_type);
	  treturn = Type::tint32;
	  break;

	case LIBCALL_HIDDEN_FUNC:
	  /* Argument is an Object, but can't use that as
	     LIBCALL_HIDDEN_FUNC is needed before the Object type is
	     created. */
	  targs.push (Type::tvoidptr);
	  break;

	default:
	  gcc_unreachable();
	}

      // Build extern(C) function.
      Identifier *id = Lexer::idPool(libcall_ids[libcall]);
      TypeFunction *tf = new TypeFunction(NULL, treturn, 0, LINKc);
      tf->varargs = varargs ? 1 : 0;

      decl = new FuncDeclaration(0, 0, id, STCstatic, tf);
      decl->protection = PROTpublic;
      decl->linkage = LINKc;

      // Add parameter types.
      Parameters *args = new Parameters;
      args->setDim (targs.dim);
      for (size_t i = 0; i < targs.dim; i++)
	(*args)[i] = new Parameter (0, targs[i], NULL, NULL);

      tf->parameters = args;
      libcall_decls[libcall] = decl;

      // These functions do not return except through catching a thrown exception.
      if (libcall == LIBCALL_ASSERT || libcall == LIBCALL_ASSERT_MSG
	  || libcall == LIBCALL_UNITTEST || libcall == LIBCALL_UNITTEST_MSG
	  || libcall == LIBCALL_ARRAY_BOUNDS || libcall == LIBCALL_SWITCH_ERROR)
	TREE_THIS_VOLATILE (decl->toSymbol()->Stree) = 1;
    }

  return decl;
}

// Build call to LIBCALL. N_ARGS is the number of call arguments which are
// specified in as a tree array ARGS.  The caller can force the return type
// of the call to FORCE_TYPE if the library call returns a generic value.

tree
build_libcall (LibCall libcall, unsigned n_args, tree *args, tree force_type)
{
  FuncDeclaration *lib_decl = get_libcall (libcall);
  Type *type = lib_decl->type->nextOf();
  tree callee = build_address (lib_decl->toSymbol()->Stree);
  tree arg_list = NULL_TREE;

  for (int i = n_args - 1; i >= 0; i--)
    arg_list = tree_cons (NULL_TREE, args[i], arg_list);

  tree result = d_build_call (type->toCtype(), callee, arg_list);

  // for TYPE, assumes caller knows what it is doing %%
  if (force_type != NULL_TREE)
    return convert (force_type, result);

  return result;
}

// Build a call to CALLEE, passing ARGS as arguments.
// The expected return type is TYPE.
// TREE_SIDE_EFFECTS gets set depending on the const/pure attributes
// of the funcion and the SIDE_EFFECTS flags of the arguments.

tree
d_build_call (tree type, tree callee, tree args)
{
  int nargs = list_length (args);
  tree *pargs = new tree[nargs];
  for (size_t i = 0; args; args = TREE_CHAIN (args), i++)
    pargs[i] = TREE_VALUE (args);

  return build_call_array (type, callee, nargs, pargs);
}

// Conveniently construct the function arguments for passing
// to the real d_build_call function.

tree
d_build_call_nary (tree callee, int n_args, ...)
{
  va_list ap;
  tree arg_list = NULL_TREE;
  tree fntype = TREE_TYPE (callee);

  va_start (ap, n_args);
  for (int i = n_args - 1; i >= 0; i--)
    arg_list = tree_cons (NULL_TREE, va_arg (ap, tree), arg_list);
  va_end (ap);

  return d_build_call (TREE_TYPE (fntype), build_address (callee), nreverse (arg_list));
}

// If CALL_EXP is a BUILT_IN_FRONTEND, expand and return inlined
// compiler generated instructions. Most map onto GCC builtins,
// others require a little extra work around them.

tree
IRState::maybeExpandSpecialCall (tree call_exp)
{
  // More code duplication from C
  CallExpr ce (call_exp);
  tree callee = ce.callee();
  tree op1 = NULL_TREE, op2 = NULL_TREE;
  tree exp = NULL_TREE, val;
  enum tree_code code;

  if (POINTER_TYPE_P (TREE_TYPE (callee)))
    callee = TREE_OPERAND (callee, 0);

  if (TREE_CODE (callee) == FUNCTION_DECL
      && DECL_BUILT_IN_CLASS (callee) == BUILT_IN_FRONTEND)
    {
      Intrinsic intrinsic = (Intrinsic) DECL_FUNCTION_CODE (callee);
      tree type;
      Type *d_type;
      switch (intrinsic)
	{
	case INTRINSIC_BSF:
	  /* builtin count_trailing_zeros matches behaviour of bsf.
	     %% TODO: The return value is supposed to be undefined if op1 is zero. */
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_CTZL), 1, op1);

	case INTRINSIC_BSR:
	  /* bsr becomes 31-(clz), but parameter passed to bsf may not be a 32bit type!!
	     %% TODO: The return value is supposed to be undefined if op1 is zero. */
	  op1 = ce.nextArg();
	  type = TREE_TYPE (op1);

	  op2 = build_integer_cst (tree_low_cst (TYPE_SIZE (type), 1) - 1, type);
	  exp = d_build_call_nary (builtin_decl_explicit (BUILT_IN_CLZL), 1, op1);

	  // Handle int -> long conversions.
	  if (TREE_TYPE (exp) != type)
	    exp = fold_convert (type, exp);

	  return fold_build2 (MINUS_EXPR, type, op2, exp);

	case INTRINSIC_BTC:
	case INTRINSIC_BTR:
	case INTRINSIC_BTS:
	  op1 = ce.nextArg();
	  op2 = ce.nextArg();
	  type = TREE_TYPE (TREE_TYPE (op1));

	  exp = build_integer_cst (tree_low_cst (TYPE_SIZE (type), 1), type);

	  // op1[op2 / exp]
	  op1 = pointerIntSum (op1, fold_build2 (TRUNC_DIV_EXPR, type, op2, exp));
	  op1 = indirect_ref (type, op1);

	  // mask = 1 << (op2 % exp);
	  op2 = fold_build2 (TRUNC_MOD_EXPR, type, op2, exp);
	  op2 = fold_build2 (LSHIFT_EXPR, type, size_one_node, op2);

	  // cond = op1[op2 / size] & mask;
	  exp = fold_build2 (BIT_AND_EXPR, type, op1, op2);

	  // cond ? -1 : 0;
	  exp = build3 (COND_EXPR, TREE_TYPE (call_exp), d_truthvalue_conversion (exp),
			integer_minus_one_node, integer_zero_node);
	  
	  // Update the bit as needed.
	  code = (intrinsic == INTRINSIC_BTC) ? BIT_XOR_EXPR :
	    (intrinsic == INTRINSIC_BTR) ? BIT_AND_EXPR :
	    (intrinsic == INTRINSIC_BTS) ? BIT_IOR_EXPR : ERROR_MARK;
	  gcc_assert (code != ERROR_MARK);

	  // op1[op2 / size] op= mask
	  if (intrinsic == INTRINSIC_BTR)
	    op2 = build1 (BIT_NOT_EXPR, TREE_TYPE (op2), op2);

	  val = localVar (TREE_TYPE (call_exp));
	  exp = vmodify_expr (val, exp);
	  op1 = vmodify_expr (op1, fold_build2 (code, TREE_TYPE (op1), op1, op2));
	  return compound_expr (exp, compound_expr (op1, val));

	case INTRINSIC_BSWAP:
	  /* Backend provides builtin bswap32.
	     Assumes first argument and return type is uint. */
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_BSWAP32), 1, op1);

	case INTRINSIC_COS:
	  // Math intrinsics just map to their GCC equivalents.
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_COSL), 1, op1);

	case INTRINSIC_SIN:
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_SINL), 1, op1);

	case INTRINSIC_RNDTOL:
	  // %% not sure if llroundl stands as a good replacement
	  // for the expected behaviour of rndtol.
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_LLROUNDL), 1, op1);

	case INTRINSIC_SQRT:
	  // Have float, double and real variants of sqrt.
	  op1 = ce.nextArg();
	  type = TREE_TYPE (op1);
	  // Could have used mathfn_built_in, but that only returns
	  // implicit built in decls.
	  if (TYPE_MAIN_VARIANT (type) == double_type_node)
	    exp = builtin_decl_explicit (BUILT_IN_SQRT);
	  else if (TYPE_MAIN_VARIANT (type) == float_type_node)
	    exp = builtin_decl_explicit (BUILT_IN_SQRTF);
	  else if (TYPE_MAIN_VARIANT (type) == long_double_type_node)
	    exp = builtin_decl_explicit (BUILT_IN_SQRTL);
	  // op1 is an integral type - use double precision.
	  else if (INTEGRAL_TYPE_P (TYPE_MAIN_VARIANT (type)))
	    {
	      op1 = convert (double_type_node, op1);
	      exp = builtin_decl_explicit (BUILT_IN_SQRT);
	    }

	  gcc_assert (exp);    // Should never trigger.
	  return d_build_call_nary (exp, 1, op1);

	case INTRINSIC_LDEXP:
	  op1 = ce.nextArg();
	  op2 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_LDEXPL), 2, op1, op2);

	case INTRINSIC_FABS:
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_FABSL), 1, op1);

	case INTRINSIC_RINT:
	  op1 = ce.nextArg();
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_RINTL), 1, op1);

	case INTRINSIC_VA_ARG:
	case INTRINSIC_C_VA_ARG:
	  op1 = ce.nextArg();
	  STRIP_NOPS (op1);

	  if (TREE_CODE (op1) == ADDR_EXPR)
	    op1 = TREE_OPERAND (op1, 0);

	  if (intrinsic == INTRINSIC_C_VA_ARG)
	    type = TREE_TYPE (TREE_TYPE (callee));
	  else
	    {
	      op2 = ce.nextArg();
	      STRIP_NOPS (op2);
	      gcc_assert (TREE_CODE (op2) == ADDR_EXPR);
	      op2 = TREE_OPERAND (op2, 0);
	      type = TREE_TYPE (op2);
	    }

	  d_type = build_dtype (type);
	  if (flag_split_darrays
	      && (d_type && d_type->toBasetype()->ty == Tarray))
	    {
	      /* should create a temp var of type TYPE and move the binding
		 to outside this expression.  */
	      tree ltype = TREE_TYPE (TYPE_FIELDS (type));
	      tree ptype = TREE_TYPE (TREE_CHAIN (TYPE_FIELDS (type)));
	      tree lvar = exprVar (ltype);
	      tree pvar = exprVar (ptype);

	      op1 = stabilize_reference (op1);

	      tree e1 = vmodify_expr (lvar, build1 (VA_ARG_EXPR, ltype, op1));
	      tree e2 = vmodify_expr (pvar, build1 (VA_ARG_EXPR, ptype, op1));
	      tree val = d_array_value (type, lvar, pvar);

	      exp = compound_expr (compound_expr (e1, e2), val);
	      exp = bind_expr (lvar, bind_expr (pvar, exp));
	    }
	  else
	    {
	      tree type2 = lang_hooks.types.type_promotes_to (type);
	      exp = build1 (VA_ARG_EXPR, type2, op1);
	      // silently convert promoted type...
	      if (type != type2)
		exp = convert (type, exp);
	    }

	  if (intrinsic == INTRINSIC_VA_ARG)
	    exp = vmodify_expr (op2, exp);

	  return exp;

	case INTRINSIC_VA_START:
	  /* The va_list argument should already have its
	     address taken.  The second argument, however, is
	     inout and that needs to be fixed to prevent a warning.  */
	  op1 = ce.nextArg();
	  op2 = ce.nextArg();
	  type = TREE_TYPE (op1);

	  // could be casting... so need to check type too?
	  STRIP_NOPS (op1);
	  STRIP_NOPS (op2);
	  gcc_assert (TREE_CODE (op1) == ADDR_EXPR
		      && TREE_CODE (op2) == ADDR_EXPR);

	  op2 = TREE_OPERAND (op2, 0);
	  // assuming nobody tries to change the return type
	  return d_build_call_nary (builtin_decl_explicit (BUILT_IN_VA_START), 2, op1, op2);

	default:
	  gcc_unreachable();
	}
    }

  return call_exp;
}

// Build and return the correct call to fmod depending on TYPE.
// ARG0 and ARG1 are the arguments pass to the function.

tree
IRState::floatMod (tree type, tree arg0, tree arg1)
{
  tree fmodfn = NULL_TREE;
  tree basetype = type;

  if (COMPLEX_FLOAT_TYPE_P (basetype))
    basetype = TREE_TYPE (basetype);

  if (TYPE_MAIN_VARIANT (basetype) == double_type_node)
    fmodfn = builtin_decl_explicit (BUILT_IN_FMOD);
  else if (TYPE_MAIN_VARIANT (basetype) == float_type_node)
    fmodfn = builtin_decl_explicit (BUILT_IN_FMODF);
  else if (TYPE_MAIN_VARIANT (basetype) == long_double_type_node)
    fmodfn = builtin_decl_explicit (BUILT_IN_FMODL);

  if (!fmodfn)
    {
      // %qT pretty prints the tree type.
      ::error ("tried to perform floating-point modulo division on %qT", type);
      return error_mark_node;
    }

  if (COMPLEX_FLOAT_TYPE_P (type))
    return build2 (COMPLEX_EXPR, type,
		   d_build_call_nary (fmodfn, 2, real_part (arg0), arg1),
		   d_build_call_nary (fmodfn, 2, imaginary_part (arg0), arg1));

  if (SCALAR_FLOAT_TYPE_P (type))
    return d_build_call_nary (fmodfn, 2, arg0, arg1);

  // Should have caught this above.
  gcc_unreachable();
}

// Returns typeinfo reference for type T.

tree
IRState::typeinfoReference (Type *t)
{
  tree ti_ref = t->getInternalTypeInfo (NULL)->toElem (this);
  gcc_assert (POINTER_TYPE_P (TREE_TYPE (ti_ref)));
  return ti_ref;
}

// Checks if DECL is an intrinsic or runtime library function that
// requires special processing.  Marks the generated trees for DECL
// as BUILT_IN_FRONTEND so can be identified later.

void
maybe_set_builtin_frontend (FuncDeclaration *decl)
{
  if (!decl->ident)
    return;

  LibCall libcall = (LibCall) binary (decl->ident->string, libcall_ids, LIBCALL_count);

  if (libcall != LIBCALL_NONE)
    {
      // It's a runtime library function, add to libcall_decls.
      if (libcall_decls[libcall] == decl)
	return;

      TypeFunction *tf = (TypeFunction *) decl->type;
      if (tf->parameters == NULL)
	{
	  FuncDeclaration *new_decl = get_libcall (libcall);
	  new_decl->toSymbol();

	  decl->type = new_decl->type;
	  decl->csym = new_decl->csym;
	}

      libcall_decls[libcall] = decl;
    }
  else
    {
      // Check if it's a front-end builtin.
      Dsymbol *dsym = decl->toParent();
      Module *mod;

      if (dsym == NULL)
	return;

      mod = dsym->getModule();

      if (is_intrinsic_module_p (mod))
	{
	  // Matches order of Intrinsic enum
	  static const char *intrinsic_names[] = {
	      "bsf", "bsr", "bswap",
	      "btc", "btr", "bts",
	  };
	  const size_t sz = sizeof (intrinsic_names) / sizeof (char *);
	  int i = binary (decl->ident->string, intrinsic_names, sz);

	  if (i == -1)
	    return;

	  // Make sure 'i' is within the range we require.
	  gcc_assert (i >= INTRINSIC_BSF && i <= INTRINSIC_BTS);
	  tree t = decl->toSymbol()->Stree;

	  DECL_BUILT_IN_CLASS (t) = BUILT_IN_FRONTEND;
	  DECL_FUNCTION_CODE (t) = (built_in_function) i;
	}
      else if (is_math_module_p (mod))
	{
	  // Matches order of Intrinsic enum
	  static const char *math_names[] = {
	      "cos", "fabs", "ldexp",
	      "rint", "rndtol", "sin",
	      "sqrt",
	  };
	  const size_t sz = sizeof (math_names) / sizeof (char *);
	  int i = binary (decl->ident->string, math_names, sz);

	  if (i == -1)
	    return;

	  // Adjust 'i' for this range of enums
	  i += INTRINSIC_COS;
	  gcc_assert (i >= INTRINSIC_COS && i <= INTRINSIC_SQRT);
	  tree t = decl->toSymbol()->Stree;

	  // rndtol returns a long type, sqrt any float type,
	  // every other math builtin returns a real type.
	  Type *tf = decl->type->nextOf();
	  if ((i == INTRINSIC_RNDTOL && tf->ty == Tint64)
	      || (i == INTRINSIC_SQRT && tf->isreal())
	      || (i != INTRINSIC_RNDTOL && tf->ty == Tfloat80))
	    {
	      DECL_BUILT_IN_CLASS (t) = BUILT_IN_FRONTEND;
	      DECL_FUNCTION_CODE (t) = (built_in_function) i;
	    }
	}
      else
	{
	  TemplateInstance *ti = dsym->isTemplateInstance();

	  if (ti == NULL)
	    return;

	  tree t = decl->toSymbol()->Stree;

	  if (is_builtin_va_arg_p (ti->tempdecl, false))
	    {
	      DECL_BUILT_IN_CLASS (t) = BUILT_IN_FRONTEND;
	      DECL_FUNCTION_CODE (t) = (built_in_function) INTRINSIC_VA_ARG;
	    }
	  else if (is_builtin_va_arg_p (ti->tempdecl, true))
	    {
	      DECL_BUILT_IN_CLASS (t) = BUILT_IN_FRONTEND;
	      DECL_FUNCTION_CODE (t) = (built_in_function) INTRINSIC_C_VA_ARG;
	    }
	  else if (is_builtin_va_start_p (ti->tempdecl))
	    {
	      DECL_BUILT_IN_CLASS (t) = BUILT_IN_FRONTEND;
	      DECL_FUNCTION_CODE (t) = (built_in_function) INTRINSIC_VA_START;
	    }
	}
    }
}

// Build and return D's internal exception Object.
// Different from the generic exception pointer.

tree
build_exception_object (void)
{
  tree obj_type = build_object_type()->toCtype();

  if (TREE_CODE (TREE_TYPE (obj_type)) == REFERENCE_TYPE)
    obj_type = TREE_TYPE (obj_type);

  // Like Java, the actual D exception object is one
  // pointer behind the exception header
  tree eh = d_build_call_nary (builtin_decl_explicit (BUILT_IN_EH_POINTER),
			       1, integer_zero_node);

  // treat exception header as (Object *)
  eh = build1 (NOP_EXPR, build_pointer_type (obj_type), eh);
  eh = build_offset_op (MINUS_EXPR, eh, TYPE_SIZE_UNIT (TREE_TYPE (eh)));

  return build1 (INDIRECT_REF, obj_type, eh);
}

// Build LABEL_DECL for IDENT given.

tree
IRState::label (Loc loc, Identifier *ident)
{
  tree t_label = build_decl (UNKNOWN_LOCATION, LABEL_DECL,
			     ident ? get_identifier (ident->string) : NULL_TREE, void_type_node);
  DECL_CONTEXT (t_label) = current_function_decl;
  DECL_MODE (t_label) = VOIDmode;
  if (loc.filename)
    object_file->setDeclLoc (t_label, loc);
  return t_label;
}

// Entry points for protected getFrameForSymbol.

tree
IRState::getFrameForFunction (FuncDeclaration *f)
{
  if (f->fbody)
    return getFrameForSymbol (f);
  else
    {
      // Should instead error on line that references f
      f->error ("nested function missing body");
      return d_null_pointer;
    }
}

tree
IRState::getFrameForNestedClass (ClassDeclaration *c)
{
  return getFrameForSymbol (c);
}

tree
IRState::getFrameForNestedStruct (StructDeclaration *s)
{
  return getFrameForSymbol (s);
}

// If NESTED_SYM is a nested function, return the static chain to be
// used when invoking that function.

// If NESTED_SYM is a nested class or struct, return the static chain
// to be used when creating an instance of the class.

// This method is protected to enforce the type checking of getFrameForFunction,
// getFrameForNestedClass, and getFrameForNestedStruct.
// getFrameForFunction also checks that the nested function is properly defined.

tree
IRState::getFrameForSymbol (Dsymbol *nested_sym)
{
  FuncDeclaration *nested_func = NULL;
  FuncDeclaration *outer_func = NULL;

  if ((nested_func = nested_sym->isFuncDeclaration()))
    {
      // gcc_assert (nested_func->isNested())
      outer_func = nested_func->toParent2()->isFuncDeclaration();
      gcc_assert (outer_func != NULL);

      if (this->func != outer_func)
	{
	  Dsymbol *this_func = this->func;
	  if (!this->func->vthis) // if no frame pointer for this function
	    {
	      nested_sym->error ("is a nested function and cannot be accessed from %s", this->func->toChars());
	      return d_null_pointer;
	    }
	  /* Make sure we can get the frame pointer to the outer function,
	     else we'll ICE later in tree-ssa.  */
	  while (nested_func != this_func)
	    {
	      FuncDeclaration *fd;
	      ClassDeclaration *cd;
	      StructDeclaration *sd;

	      // Special case for __ensure and __require.
	      if (nested_func->ident == Id::ensure || nested_func->ident == Id::require)
		{
		  outer_func = this->func;
		  break;
		}

	      if ((fd = this_func->isFuncDeclaration()))
		{
		  if (outer_func == fd->toParent2())
		    break;
		  gcc_assert (fd->isNested() || fd->vthis);
		}
	      else if ((cd = this_func->isClassDeclaration()))
		{
		  if (!cd->isNested() || !cd->vthis)
		    goto cannot_get_frame;
		  if (outer_func == cd->toParent2())
		    break;
		}
	      else if ((sd = this_func->isStructDeclaration()))
		{
		  if (!sd->isNested() || !sd->vthis)
		    goto cannot_get_frame;
		  if (outer_func == sd->toParent2())
		    break;
		}
	      else
		{
	    cannot_get_frame:
		  this->func->error ("cannot get frame pointer to %s", nested_sym->toChars());
		  return d_null_pointer;
		}
	      this_func = this_func->toParent2();
	    }
	}
    }
  else
    {
      /* It's a class (or struct).  NewExp::toElem has already determined its
	 outer scope is not another class, so it must be a function. */

      Dsymbol *sym = nested_sym;

      while (sym && !(outer_func = sym->isFuncDeclaration()))
	sym = sym->toParent2();

      /* Make sure we can access the frame of outer_func.  */
      if (outer_func != this->func)
	{
	  Dsymbol *o = nested_func = this->func;
	  do {
	      if (!nested_func->isNested())
		{
		  if (!nested_func->isMember2())
		    goto cannot_access_frame;
		}
	      while ((o = o->toParent2()))
		{
		  if ((nested_func = o->isFuncDeclaration()))
		    break;
		}
	  } while (o && o != outer_func);

	  if (!o)
	    {
	cannot_access_frame:
	      error ("cannot access frame of function '%s' from '%s'",
		     outer_func->toChars(), this->func->toChars());
	      return d_null_pointer;
	    }
	}
    }

  if (!outer_func)
    outer_func = nested_func->toParent2()->isFuncDeclaration();
  gcc_assert (outer_func != NULL);

  FuncFrameInfo *ffo = getFrameInfo (outer_func);
  if (ffo->creates_frame || ffo->static_chain)
    return getFrameRef (outer_func);

  return d_null_pointer;
}

// Starting from the current function, try to find a suitable value of
// 'this' in nested function instances.

// A suitable 'this' value is an instance of OCD or a class that has
// OCD as a base.

tree
IRState::findThis (ClassDeclaration *ocd)
{
  FuncDeclaration *fd = func;

  while (fd)
    {
      AggregateDeclaration *ad = fd->isThis();
      ClassDeclaration *cd = ad ? ad->isClassDeclaration() : NULL;

      if (cd != NULL)
	{
	  if (ocd == cd)
	    return var (fd->vthis);
	  else if (ocd->isBaseOf (cd, NULL))
	    return convertTo (var (fd->vthis), cd->type, ocd->type);
	  else
	    fd = isClassNestedInFunction (cd);
	}
      else
	{
	  if (fd->isNested())
	    fd = fd->toParent2()->isFuncDeclaration();
	  else
	    fd = NULL;
	}
    }
  return NULL_TREE;
}

// Return the outer class/struct 'this' value.
// This is here mostly due to removing duplicate code,
// and clean implementation purposes.

tree
IRState::getVThis (Dsymbol *decl, Expression *e)
{
  ClassDeclaration *cd = decl->isClassDeclaration();
  StructDeclaration *sd = decl->isStructDeclaration();

  tree vthis_value = d_null_pointer;

  if (cd)
    {
      Dsymbol *outer = cd->toParent2();
      ClassDeclaration *cdo = outer->isClassDeclaration();
      FuncDeclaration *fdo = outer->isFuncDeclaration();

      if (cdo)
	{
	  vthis_value = findThis (cdo);
	  if (vthis_value == NULL_TREE)
	    e->error ("outer class %s 'this' needed to 'new' nested class %s",
		      cdo->toChars(), cd->toChars());
	}
      else if (fdo)
	{
	  /* If a class nested in a function has no methods
	     and there are no other nested functions,
	     lower_nested_functions is never called and any
	     STATIC_CHAIN_EXPR created here will never be
	     translated. Use a null pointer for the link in
	     this case. */
	  FuncFrameInfo *ffo = getFrameInfo (fdo);
	  if (ffo->creates_frame || ffo->static_chain
	      || fdo->hasNestedFrameRefs())
	    vthis_value = getFrameForNestedClass (cd);
	  else if (fdo->vthis && fdo->vthis->type != Type::tvoidptr)
	    vthis_value = var (fdo->vthis);
	  else
	    vthis_value = d_null_pointer;
	}
      else
	gcc_unreachable();
    }
  else if (sd)
    {
      Dsymbol *outer = sd->toParent2();
      ClassDeclaration *cdo = outer->isClassDeclaration();
      FuncDeclaration *fdo = outer->isFuncDeclaration();

      if (cdo)
	{
	  vthis_value = findThis (cdo);
	  if (vthis_value == NULL_TREE)
	    e->error ("outer class %s 'this' needed to create nested struct %s",
		      cdo->toChars(), sd->toChars());
	}
      else if (fdo)
	{
	  FuncFrameInfo *ffo = getFrameInfo (fdo);
	  if (ffo->creates_frame || ffo->static_chain
	      || fdo->hasNestedFrameRefs())
	    vthis_value = getFrameForNestedStruct (sd);
	  else if (fdo->vthis && fdo->vthis->type != Type::tvoidptr)
	    vthis_value = var (fdo->vthis);
	  else
	    vthis_value = d_null_pointer;
	}
      else
	gcc_unreachable();
    }

  return vthis_value;
}

// Return the parent function of a nested class CD.

FuncDeclaration *
IRState::isClassNestedInFunction (ClassDeclaration *cd)
{
  FuncDeclaration *fd = NULL;
  while (cd && cd->isNested())
    {
      Dsymbol *dsym = cd->toParent2();
      if ((fd = dsym->isFuncDeclaration()))
	return fd;
      else
	cd = dsym->isClassDeclaration();
    }
  return NULL;
}

// Return the parent function of a nested struct SD.

FuncDeclaration *
IRState::isStructNestedInFunction (StructDeclaration *sd)
{
  FuncDeclaration *fd = NULL;
  while (sd && sd->isNested())
    {
      Dsymbol *dsym = sd->toParent2();
      if ((fd = dsym->isFuncDeclaration()))
	return fd;
      else
	sd = dsym->isStructDeclaration();
    }
  return NULL;
}


// Build static chain decl for FUNC to be passed to nested functions in D.

void
IRState::buildChain (FuncDeclaration *func)
{
  FuncFrameInfo *ffi = getFrameInfo (func);

  if (ffi->is_closure)
    {
      // Build closure pointer, which is initialised on heap.
      func->buildClosure (this);
      return;
    }

  if (!ffi->creates_frame)
    {
      if (ffi->static_chain)
	{
	  tree link = chainLink();
	  useChain (func, link);
	}
      return;
    }

  tree frame_rec_type = buildFrameForFunction (func);
  gcc_assert(COMPLETE_TYPE_P (frame_rec_type));

  tree frame_decl = localVar (frame_rec_type);
  DECL_NAME (frame_decl) = get_identifier ("__frame");
  DECL_IGNORED_P (frame_decl) = 0;
  expandDecl (frame_decl);

  // set the first entry to the parent frame, if any
  tree chain_link = chainLink();
  tree chain_field = component_ref (frame_decl, TYPE_FIELDS (frame_rec_type));

  if (chain_link == NULL_TREE)
    chain_link = d_null_pointer;

  tree chain_expr = vmodify_expr (chain_field, chain_link);
  addExp (chain_expr);

  // copy parameters that are referenced nonlocally
  for (size_t i = 0; i < func->closureVars.dim; i++)
    {
      VarDeclaration *v = func->closureVars[i];
      if (!v->isParameter())
	continue;

      Symbol *vsym = v->toSymbol();

      tree frame_field = component_ref (frame_decl, vsym->SframeField);
      tree frame_expr = vmodify_expr (frame_field, vsym->Stree);
      addExp (frame_expr);
    }

  useChain (this->func, build_address (frame_decl));
}

tree
IRState::buildFrameForFunction (FuncDeclaration *func)
{
  FuncFrameInfo *ffi = getFrameInfo (func);

  if (ffi->frame_rec != NULL_TREE)
    return ffi->frame_rec;

  tree frame_rec_type = make_node (RECORD_TYPE);
  char *name = concat (ffi->is_closure ? "CLOSURE." : "FRAME.",
		       func->toPrettyChars(), NULL);
  TYPE_NAME (frame_rec_type) = get_identifier (name);
  free (name);

  tree ptr_field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
			       get_identifier ("__chain"), ptr_type_node);
  DECL_CONTEXT (ptr_field) = frame_rec_type;

  ListMaker fields;
  fields.chain (ptr_field);

  if (!ffi->is_closure)
    {
      /* __ensure never becomes a closure, but could still be referencing parameters
	 of the calling function.  So we add all parameters as nested refs. This is
	 written as such so that all parameters appear at the front of the frame so
	 that overriding methods match the same layout when inheriting a contract.  */
      if (global.params.useOut && func->fensure)
	{
	  for (size_t i = 0; func->parameters && i < func->parameters->dim; i++)
	    {
	      VarDeclaration *v = (*func->parameters)[i];
	      // Remove if already in closureVars so can push to front.
	      for (size_t j = i; j < func->closureVars.dim; j++)
		{
		  Dsymbol *s = func->closureVars[j];
		  if (s == v)
		    {
		      func->closureVars.remove (j);
		      break;
		    }
		}
	      func->closureVars.insert (i, v);
	    }

	  // Also add hidden 'this' to outer context.
	  if (func->vthis)
	    {
	      for (size_t i = 0; i < func->closureVars.dim; i++)
		{
		  Dsymbol *s = func->closureVars[i];
		  if (s == func->vthis)
		    {
		      func->closureVars.remove (i);
		      break;
		    }
		}
	      func->closureVars.insert (0, func->vthis);
	    }
	}
    }

  for (size_t i = 0; i < func->closureVars.dim; i++)
    {
      VarDeclaration *v = func->closureVars[i];
      Symbol *s = v->toSymbol();
      tree field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
			       v->ident ? get_identifier (v->ident->string) : NULL_TREE,
			       declaration_type (v));
      s->SframeField = field;
      object_file->setDeclLoc (field, v);
      DECL_CONTEXT (field) = frame_rec_type;
      fields.chain (field);
      TREE_USED (s->Stree) = 1;

      /* Can't do nrvo if the variable is put in a frame.  */
      if (func->nrvo_can && func->nrvo_var == v)
	func->nrvo_can = 0;
    }
  TYPE_FIELDS (frame_rec_type) = fields.head;
  layout_type (frame_rec_type);
  d_keep (frame_rec_type);

  return frame_rec_type;
}

// Return the frame of FD.  This could be a static chain or a closure
// passed via the hidden 'this' pointer.

FuncFrameInfo *
IRState::getFrameInfo (FuncDeclaration *fd)
{
  Symbol *fds = fd->toSymbol();
  if (fds->frameInfo)
    return fds->frameInfo;

  FuncFrameInfo *ffi = new FuncFrameInfo;
  ffi->creates_frame = false;
  ffi->static_chain = false;
  ffi->is_closure = false;
  ffi->frame_rec = NULL_TREE;

  fds->frameInfo = ffi;

  // Nested functions, or functions with nested refs must create
  // a static frame for local variables to be referenced from.
  if (fd->closureVars.dim != 0)
    ffi->creates_frame = true;

  if (fd->vthis && fd->vthis->type == Type::tvoidptr)
    ffi->creates_frame = true;

  // Functions with In/Out contracts pass parameters to nested frame.
  if (fd->fensure || fd->frequire)
    ffi->creates_frame = true;

  // D2 maybe setup closure instead.
  if (fd->needsClosure())
    {
      ffi->creates_frame = true;
      ffi->is_closure = true;
    }
  else if (fd->closureVars.dim == 0)
    {
      /* If fd is nested (deeply) in a function that creates a closure,
	 then fd inherits that closure via hidden vthis pointer, and
	 doesn't create a stack frame at all.  */
      FuncDeclaration *ff = fd;

      while (ff)
	{
	  FuncFrameInfo *ffo = getFrameInfo (ff);
	  AggregateDeclaration *ad;

	  if (ff != fd && ffo->creates_frame)
	    {
	      gcc_assert (ffo->frame_rec);
	      ffi->creates_frame = false;
	      ffi->static_chain = true;
	      ffi->is_closure = ffo->is_closure;
	      gcc_assert (COMPLETE_TYPE_P (ffo->frame_rec));
	      ffi->frame_rec = copy_node (ffo->frame_rec);
	      break;
	    }

	  // Stop looking if no frame pointer for this function.
	  if (ff->vthis == NULL)
	    break;

	  ad = ff->isThis();
	  if (ad && ad->isNested())
	    {
	      while (ad->isNested())
		{
		  Dsymbol *d = ad->toParent2();
		  ad = d->isAggregateDeclaration();
		  ff = d->isFuncDeclaration();

		  if (ad == NULL)
		    break;
		}
	    }
	  else
	    ff = ff->toParent2()->isFuncDeclaration();
	}
    }

  // Build type now as may be referenced from another module.
  if (ffi->creates_frame)
    ffi->frame_rec = buildFrameForFunction (fd);

  return ffi;
}

// Return a pointer to the frame/closure block of OUTER_FUNC.

tree
IRState::getFrameRef (FuncDeclaration *outer_func)
{
  tree result = chainLink();
  FuncDeclaration *fd = chainFunc();

  while (fd && fd != outer_func)
    {
      AggregateDeclaration *ad;
      ClassDeclaration *cd;
      StructDeclaration *sd;

      if (getFrameInfo (fd)->creates_frame)
	{
	  // like compon (indirect, field0) parent frame link is the first field;
	  result = indirect_ref (ptr_type_node, result);
	}

      if (fd->isNested())
	fd = fd->toParent2()->isFuncDeclaration();
      /* getFrameRef is only used to get the pointer to a function's frame
	 (not a class instances.)  With the current implementation, the link
	 the frame/closure record always points to the outer function's frame even
	 if there are intervening nested classes or structs.
	 So, we can just skip over those... */
	 else if ((ad = fd->isThis()) && (cd = ad->isClassDeclaration()))
	   fd = isClassNestedInFunction (cd);
	 else if ((ad = fd->isThis()) && (sd = ad->isStructDeclaration()))
	   fd = isStructNestedInFunction (sd);
	 else
	   break;
    }

  if (fd == outer_func)
    {
      tree frame_rec = getFrameInfo (outer_func)->frame_rec;

      if (frame_rec != NULL_TREE)
	{
	  result = build_nop (build_pointer_type (frame_rec), result);
	  return result;
	}
      else
	{
	  this->func->error ("forward reference to frame of %s", outer_func->toChars());
	  return d_null_pointer;
	}
    }
  else
    {
      this->func->error ("cannot access frame of %s", outer_func->toChars());
      return d_null_pointer;
    }
}

// Special case: If a function returns a nested class with functions
// but there are no "closure variables" the frontend (needsClosure) 
// returns false even though the nested class _is_ returned from the
// function. (See case 4 in needsClosure)
// A closure is strictly speaking not necessary, but we also can not
// use a static function chain for functions in the nested class as
// they can be called from outside. GCC's nested functions can't deal
// with those kind of functions. We have to detect them manually here
// and make sure we neither construct a static chain nor a closure.

bool
functionDegenerateClosure (FuncDeclaration *f)
{
  if (!f->needsClosure() && f->closureVars.dim == 0)
  {
    Type *tret = ((TypeFunction *)f->type)->next;
    gcc_assert(tret);
    tret = tret->toBasetype();
    if (tret->ty == Tclass || tret->ty == Tstruct)
    { 
      Dsymbol *st = tret->toDsymbol(NULL);
      for (Dsymbol *s = st->parent; s; s = s->parent)
      {
	if (s == f)
	  return true;
      }
    }
  }
  return false;
}

// Return true if function F needs to have the static chain passed to
// it.  This only applies to nested function handling provided by the
// GCC back end (not D closures.)

bool
IRState::functionNeedsChain (FuncDeclaration *f)
{
  Dsymbol *s;
  FuncDeclaration *pf = NULL;
  TemplateInstance *ti = NULL;

  if (f->isNested())
    {
      s = f->toParent();
      ti = s->isTemplateInstance();
      if (ti && ti->isnested == NULL && ti->parent->isModule())
	return false;

      pf = f->toParent2()->isFuncDeclaration();
      if (pf && !getFrameInfo (pf)->is_closure)
	return true;
    }

  if (f->isStatic())
    return false;

  s = f->toParent2();

  while (s)
    {
      AggregateDeclaration *ad = s->isAggregateDeclaration();
      if (!ad || !ad->isNested())
	break;

      if (!s->isTemplateInstance())
	break;

      s = s->toParent2();
      if ((pf = s->isFuncDeclaration())
	  && !getFrameInfo (pf)->is_closure
	  && !functionDegenerateClosure(pf))
	return true;
    }

  return false;
}


// Routines for building statement lists around if/else conditions.
// STMT contains the statement to be executed if T_COND is true.

void
IRState::startCond (Statement *stmt, tree t_cond)
{
  Flow *f = beginFlow (stmt);
  f->condition = t_cond;
}

void
IRState::startCond (Statement *stmt, Expression *e_cond)
{
  tree t_cond = e_cond->toElemDtor (this);
  startCond (stmt, convertForCondition (t_cond, e_cond->type));
}

// Start a new statement list for the false condition branch.

void
IRState::startElse (void)
{
  currentFlow()->trueBranch = popStatementList();
  pushStatementList();
}

// Wrap up our constructed if condition into a COND_EXPR.

void
IRState::endCond (void)
{
  Flow *f = currentFlow();
  tree t_brnch = popStatementList();
  tree t_false_brnch = NULL_TREE;

  if (f->trueBranch == NULL_TREE)
    f->trueBranch = t_brnch;
  else
    t_false_brnch = t_brnch;

  object_file->doLineNote (f->statement->loc);
  tree t_stmt = build3 (COND_EXPR, void_type_node,
			f->condition, f->trueBranch, t_false_brnch);
  endFlow();
  addExp (t_stmt);
}


// Routines for building statement lists around for/while loops.
// STMT is the body of the loop.

void
IRState::startLoop (Statement *stmt)
{
  Flow *f = beginFlow (stmt);
  // should be end for 'do' loop
  f->continueLabel = label (stmt ? stmt->loc : 0);
}

// Emit continue label for loop.

void
IRState::continueHere (void)
{
  doLabel (currentFlow()->continueLabel);
}

// Set LBL as the continue label for the current loop.
// Used in unrolled loop statements.

void
IRState::setContinueLabel (tree lbl)
{
  currentFlow()->continueLabel = lbl;
}

// Emit exit loop condition.

void
IRState::exitIfFalse (tree t_cond)
{
  addExp (build1 (EXIT_EXPR, void_type_node,
		  build1 (TRUTH_NOT_EXPR, TREE_TYPE (t_cond), t_cond)));
}

void
IRState::exitIfFalse (Expression *e_cond)
{
  tree t_cond = e_cond->toElemDtor (this);
  exitIfFalse (convertForCondition (t_cond, e_cond->type));
}

// Emit a goto to the continue label IDENT of a loop.

void
IRState::continueLoop (Identifier *ident)
{
  doJump (NULL, getLoopForLabel (ident, true)->continueLabel);
}

// Emit a goto to the exit label IDENT of a loop.

void
IRState::exitLoop (Identifier *ident)
{
  Flow *flow = getLoopForLabel (ident);
  if (!flow->exitLabel)
    flow->exitLabel = label (flow->statement->loc);
  doJump (NULL, flow->exitLabel);
}

// Wrap up constructed loop body in a LOOP_EXPR.

void
IRState::endLoop (void)
{
  // says must contain an EXIT_EXPR -- what about while (1)..goto;? something other thand LOOP_EXPR?
  tree t_body = popStatementList();
  tree t_loop = build1 (LOOP_EXPR, void_type_node, t_body);
  addExp (t_loop);
  endFlow();
}


// Routines for building statement lists around switches.  STMT is the body
// of the switch statement, T_COND is the condition to the switch. If HAS_VARS
// is true, then the switch statement has been converted to an if-then-else.

void
IRState::startCase (Statement *stmt, tree t_cond, int has_vars)
{
  Flow *f = beginFlow (stmt);
  f->condition = t_cond;
  f->kind = level_switch;
  if (has_vars)
    {
      // %% dummy value so the tree is not NULL
      f->hasVars = integer_one_node;
    }
}

// Emit a case statement for T_VALUE.

void
IRState::doCase (tree t_value, tree t_label)
{
  if (currentFlow()->hasVars)
    {
      // SwitchStatement has already taken care of label jumps.
      doLabel (t_label);
    }
  else
    {
      tree t_case = build_case_label (t_value, NULL_TREE, t_label);
      addExp (t_case);
    }
}

// Wrap up constructed body into a SWITCH_EXPR.

void
IRState::endCase (void)
{
  Flow *f = currentFlow();
  tree t_body = popStatementList();
  tree t_condtype = TREE_TYPE (f->condition);
  if (f->hasVars)
    {
      // %% switch was converted to if-then-else expression
      addExp (t_body);
    }
  else
    {
      tree t_stmt = build3 (SWITCH_EXPR, t_condtype, f->condition,
			    t_body, NULL_TREE);
      addExp (t_stmt);
    }
  endFlow();
}

// Routines for building statement lists around try/catch/finally.
// Start a try statement, STMT is the body of the try expression.

void
IRState::startTry (Statement *stmt)
{
  beginFlow (stmt);
  currentFlow()->kind = level_try;
}

// Pops the try body and starts a new statement list for all catches.

void
IRState::startCatches (void)
{
  currentFlow()->tryBody = popStatementList();
  currentFlow()->kind = level_catch;
  pushStatementList();
}

// Start a new catch expression for exception type T_TYPE.

void
IRState::startCatch (tree t_type)
{
  currentFlow()->catchType = t_type;
  pushStatementList();
}

// Wrap up catch expression into a CATCH_EXPR.

void
IRState::endCatch (void)
{
  tree t_body = popStatementList();
  // % Wrong loc... can set pass statement to startCatch, set
  // The loc on t_type and then use it here...
  addExp (build2 (CATCH_EXPR, void_type_node,
		  currentFlow()->catchType, t_body));
}

// Wrap up try/catch into a TRY_CATCH_EXPR.

void
IRState::endCatches (void)
{
  tree t_catches = popStatementList();
  object_file->doLineNote (currentFlow()->statement->loc);
  addExp (build2 (TRY_CATCH_EXPR, void_type_node,
		  currentFlow()->tryBody, t_catches));
  endFlow();
}

// Start a new finally expression.

void
IRState::startFinally (void)
{
  currentFlow()->tryBody = popStatementList();
  currentFlow()->kind = level_finally;
  pushStatementList();
}

// Wrap-up try/finally into a TRY_FINALLY_EXPR.

void
IRState::endFinally (void)
{
  tree t_finally = popStatementList();
  object_file->doLineNote (currentFlow()->statement->loc);
  addExp (build2 (TRY_FINALLY_EXPR, void_type_node,
		  currentFlow()->tryBody, t_finally));
  endFlow();
}

// Emit a return expression of value T_VALUE.

void
IRState::doReturn (tree t_value)
{
  addExp (build1 (RETURN_EXPR, void_type_node, t_value));
}

// Emit goto expression to T_LABEL.

void
IRState::doJump (Statement *stmt, tree t_label)
{
  if (stmt)
    object_file->doLineNote (stmt->loc);
  addExp (build1 (GOTO_EXPR, void_type_node, t_label));
  TREE_USED (t_label) = 1;
}

// Routines for checking goto statements don't jump to invalid locations.
// In particular, it is illegal for a goto to be used to skip initializations.
// Saves the block label L is declared in for analysis later.

void
IRState::pushLabel (LabelDsymbol *l)
{
  this->labels.push (getLabelBlock (l));
}

// Error if STMT is in it's own try statement separate from other
// cases in the switch statement.

void
IRState::checkSwitchCase (Statement *stmt, int default_flag)
{
  Flow *flow = currentFlow();

  gcc_assert (flow);
  if (flow->kind != level_switch && flow->kind != level_block)
    {
      stmt->error ("%s cannot be in different try block level from switch",
		   default_flag ? "default" : "case");
    }
}

// Error if the goto referencing LABEL is jumping into a try or
// catch block.  STMT is required to error on the correct line.

void
IRState::checkGoto (Statement *stmt, LabelDsymbol *label)
{
  Statement *curBlock = NULL;
  unsigned curLevel = this->loops.dim;
  int found = 0;

  if (curLevel)
    curBlock = currentFlow()->statement;

  for (size_t i = 0; i < this->labels.dim; i++)
    {
      Label *linfo = this->labels[i];
      gcc_assert (linfo);

      if (label == linfo->label)
	{
	  // No need checking for finally, should have already been handled.
	  if (linfo->kind == level_try
	      && curLevel <= linfo->level && curBlock != linfo->block)
	    {
	      stmt->error ("cannot goto into try block");
	    }
	  // %% doc: It is illegal for goto to be used to skip initializations,
	  // %%      so this should include all gotos into catches...
	  if (linfo->kind == level_catch && curBlock != linfo->block)
	    stmt->error ("cannot goto into catch block");

	  found = 1;
	  break;
	}
    }
  // Push forward referenced gotos.
  if (!found)
    {
      if (!label->statement->fwdrefs)
	label->statement->fwdrefs = new Blocks();
      label->statement->fwdrefs->push (getLabelBlock (label, stmt));
    }
}

// Check all forward references REFS for a label, and error
// if goto is jumping into a try or catch block.

void
IRState::checkPreviousGoto (Array *refs)
{
  Statement *stmt; // Our forward reference.

  for (size_t i = 0; i < refs->dim; i++)
    {
      Label *ref = (Label *) refs->data[i];
      int found = 0;

      gcc_assert (ref && ref->from);
      stmt = ref->from;

      for (size_t i = 0; i < this->labels.dim; i++)
	{
	  Label *linfo = this->labels[i];
	  gcc_assert (linfo);

	  if (ref->label == linfo->label)
	    {
	      // No need checking for finally, should have already been handled.
	      if (linfo->kind == level_try
		  && ref->level <= linfo->level && ref->block != linfo->block)
		{
		  stmt->error ("cannot goto into try block");
		}
	      // %% doc: It is illegal for goto to be used to skip initializations,
	      // %%      so this should include all gotos into catches...
	      if (linfo->kind == level_catch
		  && (ref->block != linfo->block || ref->kind != linfo->kind))
		stmt->error ("cannot goto into catch block");

	      found = 1;
	      break;
	    }
	}
      gcc_assert (found);
    }
}

// Construct a WrappedExp, whose components are an EXP_NODE, which contains
// a list of instructions in GCC to be passed through.

WrappedExp::WrappedExp (Loc loc, enum TOK op, tree exp_node, Type *type)
    : Expression (loc, op, sizeof (WrappedExp))
{
  this->exp_node = exp_node;
  this->type = type;
}

// Write C-style representation of WrappedExp to BUF.

void
WrappedExp::toCBuffer (OutBuffer *buf, HdrGenState *hgs ATTRIBUTE_UNUSED)
{
  buf->printf ("<wrapped expression>");
}

// Build and return expression tree for WrappedExp.

elem *
WrappedExp::toElem (IRState *)
{
  return exp_node;
}

// Write out all fields for aggregate DECL.  For classes, write
// out base class fields first, and adds all interfaces last.

void
AggLayout::visit (AggregateDeclaration *decl)
{
  ClassDeclaration *class_decl = decl->isClassDeclaration();

  if (class_decl && class_decl->baseClass)
    AggLayout::visit (class_decl->baseClass);

  if (decl->fields.dim)
    doFields (&decl->fields, decl);

  if (class_decl && class_decl->vtblInterfaces)
    doInterfaces (class_decl->vtblInterfaces);
}


// Add all FIELDS into aggregate AGG.

void
AggLayout::doFields (VarDeclarations *fields, AggregateDeclaration *agg)
{
  bool inherited = agg != this->aggDecl_;
  tree fcontext;

  fcontext = agg->type->toCtype();
  if (POINTER_TYPE_P (fcontext))
    fcontext = TREE_TYPE (fcontext);

  for (size_t i = 0; i < fields->dim; i++)
    {
      // %% D anonymous unions just put the fields into the outer struct...
      // does this cause problems?
      VarDeclaration *var_decl = (*fields)[i];
      gcc_assert (var_decl && var_decl->storage_class & STCfield);

      tree ident = var_decl->ident ? get_identifier (var_decl->ident->string) : NULL_TREE;
      tree field_decl = build_decl (UNKNOWN_LOCATION, FIELD_DECL, ident,
				    declaration_type (var_decl));
      object_file->setDeclLoc (field_decl, var_decl);
      var_decl->csym = new Symbol;
      var_decl->csym->Stree = field_decl;

      DECL_CONTEXT (field_decl) = this->aggType_;
      DECL_FCONTEXT (field_decl) = fcontext;
      DECL_FIELD_OFFSET (field_decl) = size_int (var_decl->offset);
      DECL_FIELD_BIT_OFFSET (field_decl) = bitsize_zero_node;

      DECL_ARTIFICIAL (field_decl) = DECL_IGNORED_P (field_decl) = inherited;
      SET_DECL_OFFSET_ALIGN (field_decl, TYPE_ALIGN (TREE_TYPE (field_decl)));

      layout_decl (field_decl, 0);

      TREE_THIS_VOLATILE (field_decl) = TYPE_VOLATILE (TREE_TYPE (field_decl));

      if (var_decl->size (var_decl->loc))
	{
	  gcc_assert (DECL_MODE (field_decl) != VOIDmode);
	  gcc_assert (DECL_SIZE (field_decl) != NULL_TREE);
	}
      this->fieldList_.chain (field_decl);
    }
}

// Write out all interfaces BASES for a class.

void
AggLayout::doInterfaces (BaseClasses *bases)
{
  for (size_t i = 0; i < bases->dim; i++)
    {
      BaseClass *bc = (*bases)[i];
      tree decl = build_decl (UNKNOWN_LOCATION, FIELD_DECL, NULL_TREE,
			      Type::tvoidptr->pointerTo()->toCtype());
      DECL_ARTIFICIAL (decl) = 1;
      DECL_IGNORED_P (decl) = 1;
      addField (decl, bc->offset);
    }
}

// Add single field FIELD_DECL at OFFSET into aggregate.

void
AggLayout::addField (tree field_decl, size_t offset)
{
  DECL_CONTEXT (field_decl) = this->aggType_;
  SET_DECL_OFFSET_ALIGN (field_decl, TYPE_ALIGN (TREE_TYPE (field_decl)));
  DECL_FIELD_OFFSET (field_decl) = size_int (offset);
  DECL_FIELD_BIT_OFFSET (field_decl) = bitsize_zero_node;
  Loc l (this->aggDecl_->getModule(), 1); // Must set this or we crash with DWARF debugging
  object_file->setDeclLoc (field_decl, l);

  TREE_THIS_VOLATILE (field_decl) = TYPE_VOLATILE (TREE_TYPE (field_decl));

  layout_decl (field_decl, 0);
  this->fieldList_.chain (field_decl);
}

// Wrap-up and compute finalised aggregate type.  ATTRS are
// if any GCC attributes were applied to the type declaration.

void
AggLayout::finish (Expressions *attrs)
{
  unsigned size_to_use = this->aggDecl_->structsize;
  unsigned align_to_use = this->aggDecl_->alignsize;

  TYPE_SIZE (this->aggType_) = bitsize_int (size_to_use * BITS_PER_UNIT);
  TYPE_SIZE_UNIT (this->aggType_) = size_int (size_to_use);
  TYPE_ALIGN (this->aggType_) = align_to_use * BITS_PER_UNIT;
  TYPE_PACKED (this->aggType_) = TYPE_PACKED (this->aggType_); // %% todo

  if (attrs)
    {
      decl_attributes (&this->aggType_, build_attributes (attrs),
		       ATTR_FLAG_TYPE_IN_PLACE);
    }

  compute_record_mode (this->aggType_);

  // Set up variants.
  for (tree x = TYPE_MAIN_VARIANT (this->aggType_); x; x = TYPE_NEXT_VARIANT (x))
    {
      TYPE_FIELDS (x) = TYPE_FIELDS (this->aggType_);
      TYPE_LANG_SPECIFIC (x) = TYPE_LANG_SPECIFIC (this->aggType_);
      TYPE_ALIGN (x) = TYPE_ALIGN (this->aggType_);
      TYPE_USER_ALIGN (x) = TYPE_USER_ALIGN (this->aggType_);
    }
}

// Routines for getting an index or slice of an array where '$' was used
// in the slice.  A temp var INI_V would have been created that needs to
// be bound into it's own scope.

ArrayScope::ArrayScope (IRState *irs, VarDeclaration *ini_v, const Loc& loc) :
  var_(ini_v)
{
  /* If STCconst, the temp var is not required.  */
  if (this->var_ && !(this->var_->storage_class & STCconst))
    {
      /* Need to set the location or the expand_decl in the BIND_EXPR will
	 cause the line numbering for the statement to be incorrect. */
      /* The variable itself is not included in the debugging information. */
      this->var_->loc = loc;
      Symbol *s = this->var_->toSymbol();
      tree decl = s->Stree;
      DECL_CONTEXT (decl) = irs->getLocalContext();
    }
  else
    this->var_ = NULL;
}

// Set index expression E of type T as the initialiser for
// the temp var decl to be used.

tree
ArrayScope::setArrayExp (tree e, Type *t)
{
  if (this->var_)
    {
      tree v = this->var_->toSymbol()->Stree;
      if (t->toBasetype()->ty != Tsarray)
	e = maybe_make_temp (e);
      DECL_INITIAL (v) = get_array_length (e, t);
    }
  return e;
}

// Wrap-up temp var into a BIND_EXPR.

tree
ArrayScope::finish (tree e)
{
  if (this->var_)
    {
      Symbol *s = this->var_->toSymbol();
      tree t = s->Stree;
      if (TREE_CODE (t) == VAR_DECL)
	{
	  gcc_assert (!s->SframeField);
	  return bind_expr (t, e);
	}
      else
	gcc_unreachable();
    }
  return e;
}

