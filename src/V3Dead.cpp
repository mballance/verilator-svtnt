// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Dead code elimination
//
// Code available from: http://www.veripool.org/verilator
//
//*************************************************************************
//
// Copyright 2003-2017 by Wilson Snyder.  This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
//
// Verilator is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//*************************************************************************
// DEAD TRANSFORMATIONS:
//	Remove any unreferenced modules
//	Remove any unreferenced variables
//
// TODO: A graph would make the process of circular and interlinked
// dependencies easier to resolve.
// NOTE: If redo this, consider using maybePointedTo()/broken() ish scheme
// instead of needing as many visitors.
//
// The following nodes have package pointers and are cleaned up here:
// AstRefDType, AstEnumItemRef, AstNodeVarRef, AstNodeFTask
// These have packagep but will not exist at this stage
// AstPackageImport, AstDot, AstPackageRef
//
// Note on packagep: After the V3Scope/V3LinkDotScoped stage, package links
// are no longer used, but their presence prevents us from removing empty
// packages. As the links as no longer used after V3Scope, we remove them
// here after scoping to allow more dead node
// removal.
// *************************************************************************

#include "config_build.h"
#include "verilatedos.h"
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <vector>
#include <map>

#include "V3Global.h"
#include "V3Dead.h"
#include "V3Ast.h"

//######################################################################

class DeadModVisitor : public AstNVisitor {
    // In a module that is dead, cleanup the in-use counts of the modules
private:
    // NODE STATE
    // ** Shared with DeadVisitor **
    // VISITORS
    virtual void visit(AstCell* nodep) {
	nodep->iterateChildren(*this);
	nodep->modp()->user1Inc(-1);
    }
    //-----
    virtual void visit(AstNodeMath* nodep) {}  // Accelerate
    virtual void visit(AstNode* nodep) {
	nodep->iterateChildren(*this);
    }
public:
    // CONSTRUCTORS
    explicit DeadModVisitor(AstNodeModule* nodep) {
	nodep->accept(*this);
    }
    virtual ~DeadModVisitor() {}
};

//######################################################################
// Dead state, as a visitor of each AstNode

class DeadVisitor : public AstNVisitor {
private:
    // NODE STATE
    // Entire Netlist:
    //	AstNodeModule::user1()	-> int. Count of number of cells referencing this module.
    //	AstVar::user1()		-> int. Count of number of references
    //	AstVarScope::user1()	-> int. Count of number of references
    //	AstNodeDType::user1()	-> int. Count of number of references
    AstUser1InUse	m_inuser1;

    // TYPES
    typedef multimap<AstVarScope*,AstNodeAssign*>	AssignMap;

    // STATE
    AstNodeModule*		m_modp;		// Current module
    vector<AstVar*>		m_varsp;	// List of all encountered to avoid another loop through tree
    vector<AstNode*>		m_dtypesp;	// List of all encountered to avoid another loop through tree
    vector<AstVarScope*>	m_vscsp;	// List of all encountered to avoid another loop through tree
    vector<AstScope*>		m_scopesp;	// List of all encountered to avoid another loop through tree
    vector<AstCell*>		m_cellsp;	// List of all encountered to avoid another loop through tree
    AssignMap			m_assignMap;	// List of all simple assignments for each variable
    bool			m_elimUserVars;	// Allow removal of user's vars
    bool			m_elimDTypes;	// Allow removal of DTypes
    bool			m_elimScopes;	// Allow removal of Scopes
    bool			m_elimCells;	// Allow removal of Cells
    bool			m_sideEffect;	// Side effects discovered in assign RHS

    // METHODS
    static int debug() {
	static int level = -1;
	if (VL_UNLIKELY(level < 0)) level = v3Global.opt.debugSrcLevel(__FILE__);
	return level;
    }

    void checkAll(AstNode* nodep) {
	if (nodep != nodep->dtypep()) {	 // NodeDTypes reference themselves
	    if (AstNode* subnodep = nodep->dtypep()) {
		subnodep->user1Inc();
	    }
	}
	if (AstNode* subnodep = nodep->getChildDTypep()) {
	    subnodep->user1Inc();
	}
    }
    void checkDType(AstNodeDType* nodep) {
	if (!nodep->generic()  // Don't remove generic types
	    && m_elimDTypes  // dtypes stick around until post-widthing
	    && !nodep->castMemberDType() // Keep member names iff upper type exists
	    ) {
	    m_dtypesp.push_back(nodep);
	}
	if (AstNode* subnodep = nodep->virtRefDTypep()) {
	    subnodep->user1Inc();
	}
    }

    // VISITORS
    virtual void visit(AstNodeModule* nodep) {
	m_modp = nodep;
	nodep->iterateChildren(*this);
	checkAll(nodep);
	m_modp = NULL;
    }
    virtual void visit(AstCFunc* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->scopep()) nodep->scopep()->user1Inc();
    }
    virtual void visit(AstScope* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->aboveScopep()) nodep->aboveScopep()->user1Inc();

	if (!nodep->isTop() && !nodep->varsp() && !nodep->blocksp() && !nodep->finalClksp()) {
	    m_scopesp.push_back(nodep);
	}
    }
    virtual void visit(AstCell* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	m_cellsp.push_back(nodep);
	nodep->modp()->user1Inc();
    }

    virtual void visit(AstNodeVarRef* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->varScopep()) {
	    nodep->varScopep()->user1Inc();
	    nodep->varScopep()->varp()->user1Inc();
	}
	if (nodep->varp()) {
	    nodep->varp()->user1Inc();
	}
	if (nodep->packagep()) {
	    if (m_elimCells) nodep->packagep(NULL);
	    else nodep->packagep()->user1Inc();
	}
    }
    virtual void visit(AstNodeFTaskRef* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->packagep()) {
	    if (m_elimCells) nodep->packagep(NULL);
	    else nodep->packagep()->user1Inc();
	}
    }
    virtual void visit(AstRefDType* nodep) {
	nodep->iterateChildren(*this);
	checkDType(nodep);
	checkAll(nodep);
	if (nodep->packagep()) {
	    if (m_elimCells) nodep->packagep(NULL);
	    else nodep->packagep()->user1Inc();
	}
    }
    virtual void visit(AstNodeDType* nodep) {
	nodep->iterateChildren(*this);
	checkDType(nodep);
	checkAll(nodep);
    }
    virtual void visit(AstEnumItemRef* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->packagep()) {
	    if (m_elimCells) nodep->packagep(NULL);
	    else nodep->packagep()->user1Inc();
	}
	checkAll(nodep);
    }
    virtual void visit(AstModport* nodep) {
	nodep->iterateChildren(*this);
	if (m_elimCells) {
	    if (!nodep->varsp()) {
		pushDeletep(nodep->unlinkFrBack()); VL_DANGLING(nodep);
		return;
	    }
	}
	checkAll(nodep);
    }
    virtual void visit(AstTypedef* nodep) {
	nodep->iterateChildren(*this);
	if (m_elimCells && !nodep->attrPublic()) {
	    pushDeletep(nodep->unlinkFrBack()); VL_DANGLING(nodep);
	    return;
	}
	checkAll(nodep);
	// Don't let packages with only public variables disappear
	// Normal modules may disappear, e.g. if they are parameterized then removed
	if (nodep->attrPublic() && m_modp && m_modp->castPackage()) m_modp->user1Inc();
    }
    virtual void visit(AstVarScope* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->scopep()) nodep->scopep()->user1Inc();
	if (mightElimVar(nodep->varp())) {
	    m_vscsp.push_back(nodep);
	}
    }
    virtual void visit(AstVar* nodep) {
	nodep->iterateChildren(*this);
	checkAll(nodep);
	if (nodep->isSigPublic() && m_modp && m_modp->castPackage()) m_modp->user1Inc();
	if (mightElimVar(nodep)) {
	    m_varsp.push_back(nodep);
	}
    }
    virtual void visit(AstNodeAssign* nodep) {
	// See if simple assignments to variables may be eliminated because that variable is never used.
	// Similar code in V3Life
	m_sideEffect = false;
	nodep->rhsp()->iterateAndNext(*this);
	checkAll(nodep);
	// Has to be direct assignment without any EXTRACTing.
	AstVarRef* varrefp = nodep->lhsp()->castVarRef();
	if (varrefp && !m_sideEffect
	    && varrefp->varScopep()) {	// For simplicity, we only remove post-scoping
	    m_assignMap.insert(make_pair(varrefp->varScopep(), nodep));
	    checkAll(varrefp);	// Must track reference to dtype()
	} else {  // Track like any other statement
	    nodep->lhsp()->iterateAndNext(*this);
	}
	checkAll(nodep);
    }

    //-----
    virtual void visit(AstNode* nodep) {
	if (nodep->isOutputter()) m_sideEffect=true;
	nodep->iterateChildren(*this);
	checkAll(nodep);
    }

    // METHODS
    void deadCheckMod() {
	// Kill any unused modules
	// V3LinkCells has a graph that is capable of this too, but we need to do it
	// after we've done all the generate blocks
	for (bool retry=true; retry; ) {
	    retry=false;
	    AstNodeModule* nextmodp;
	    for (AstNodeModule* modp = v3Global.rootp()->modulesp(); modp; modp=nextmodp) {
		nextmodp = modp->nextp()->castNodeModule();
		if (modp->level()>2	&& modp->user1()==0 && !modp->internal()) {
		    // > 2 because L1 is the wrapper, L2 is the top user module
		    UINFO(4,"  Dead module "<<modp<<endl);
		    // And its children may now be killable too; correct counts
		    // Recurse, as cells may not be directly under the module but in a generate
		    DeadModVisitor visitor(modp);
		    modp->unlinkFrBack()->deleteTree(); VL_DANGLING(modp);
		    retry = true;
		}
	    }
	}
    }
    bool mightElimVar(AstVar* nodep) {
	return (!nodep->isSigPublic()	// Can't elim publics!
		&& !nodep->isIO()
		&& (nodep->isTemp()
		    || (nodep->isParam() && !nodep->isTrace())
		    || m_elimUserVars));  // Post-Trace can kill most anything
    }

    void deadCheckScope() {
	for (bool retry=true; retry; ) {
	    retry = false;
	    for (vector<AstScope*>::iterator it = m_scopesp.begin(); it != m_scopesp.end();++it) {
		AstScope* scp = *it;
		if (!scp)
		    continue;
		if (scp->user1() == 0) {
		    UINFO(4, "	Dead AstScope " << scp << endl);
		    scp->aboveScopep()->user1Inc(-1);
		    if (scp->dtypep()) {
			scp->dtypep()->user1Inc(-1);
		    }
		    scp->unlinkFrBack()->deleteTree(); VL_DANGLING(scp);
		    *it = NULL;
		    retry = true;
		}
	    }
	}
    }

    void deadCheckCells() {
	for (vector<AstCell*>::iterator it = m_cellsp.begin(); it!=m_cellsp.end(); ++it) {
	    AstCell* cellp = *it;
	    if (cellp->user1() == 0 && !cellp->modp()->stmtsp()) {
		cellp->modp()->user1Inc(-1);
		cellp->unlinkFrBack()->deleteTree(); VL_DANGLING(cellp);
	    }
	}
    }

    void deadCheckVar() {
	// Delete any unused varscopes
	for (vector<AstVarScope*>::iterator it = m_vscsp.begin(); it!=m_vscsp.end(); ++it) {
	    AstVarScope* vscp = *it;
	    if (vscp->user1() == 0) {
		UINFO(4,"  Dead "<<vscp<<endl);
		pair <AssignMap::iterator,AssignMap::iterator> eqrange = m_assignMap.equal_range(vscp);
		for (AssignMap::iterator it = eqrange.first; it != eqrange.second; ++it) {
		    AstNodeAssign* assp = it->second;
		    UINFO(4,"	 Dead assign "<<assp<<endl);
		    assp->dtypep()->user1Inc(-1);
		    assp->unlinkFrBack()->deleteTree(); VL_DANGLING(assp);
		}
		if (vscp->scopep()) vscp->scopep()->user1Inc(-1);
		vscp->dtypep()->user1Inc(-1);
		vscp->unlinkFrBack()->deleteTree(); VL_DANGLING(vscp);
	    }
	}
	for (bool retry=true; retry; ) {
	    retry = false;
	    for (vector<AstVar *>::iterator it = m_varsp.begin(); it != m_varsp.end();++it) {
		AstVar* varp = *it;
		if (!varp)
		    continue;
		if (varp->user1() == 0) {
		    UINFO(4, "	Dead " << varp << endl);
		    if (varp->dtypep()) {
			varp->dtypep()->user1Inc(-1);
		    }
		    varp->unlinkFrBack()->deleteTree(); VL_DANGLING(varp);
		    *it = NULL;
		    retry = true;
		}
	    }
	}
	for (vector<AstNode*>::iterator it = m_dtypesp.begin(); it != m_dtypesp.end();++it) {
	    if ((*it)->user1() == 0) {
		AstNodeClassDType *classp;
		// It's possible that there if a reference to each individual member, but
		// not to the dtype itself.  Check and don't remove the parent dtype if
		// members are still alive.
		if ((classp = (*it)->castNodeClassDType())) {
		    bool cont = true;
		    for (AstMemberDType *memberp = classp->membersp(); memberp; memberp = memberp->nextp()->castMemberDType()) {
			if (memberp->user1() != 0) {
			    cont = false;
			    break;
			}
		    }
		    if (!cont)
			continue;
		}
		(*it)->unlinkFrBack()->deleteTree(); VL_DANGLING(*it);
	    }
	}
    }

public:
    // CONSTRUCTORS
    DeadVisitor(AstNetlist* nodep, bool elimUserVars, bool elimDTypes, bool elimScopes, bool elimCells) {
	m_modp = NULL;
	m_elimCells = elimCells;
	m_elimUserVars = elimUserVars;
	m_elimDTypes = elimDTypes;
	m_elimScopes = elimScopes;
	m_sideEffect = false;
	// Prepare to remove some datatypes
	nodep->typeTablep()->clearCache();
	// Operate on whole netlist
	nodep->accept(*this);

	deadCheckVar();
	// We only elimate scopes when in a flattened structure
	// Otherwise we have no easy way to know if a scope is used
	if (elimScopes) deadCheckScope();
	if (elimCells) deadCheckCells();
	// Modules after vars, because might be vars we delete inside a mod we delete
	deadCheckMod();

	// We may have removed some datatypes, cleanup
	nodep->typeTablep()->repairCache();
    }
    virtual ~DeadVisitor() {}
};

//######################################################################
// Dead class functions

void V3Dead::deadifyModules(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    DeadVisitor visitor (nodep, false, false, false, false);
    V3Global::dumpCheckGlobalTree("deadModules.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
}

void V3Dead::deadifyDTypes(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    DeadVisitor visitor (nodep, false, true, false, false);
    V3Global::dumpCheckGlobalTree("deadDtypes.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3Dead::deadifyDTypesScoped(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    DeadVisitor visitor (nodep, false, true, true, false);
    V3Global::dumpCheckGlobalTree("deadDtypesScoped.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3Dead::deadifyAll(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    DeadVisitor visitor (nodep, true, true, false, true);
    V3Global::dumpCheckGlobalTree("deadAll.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3Dead::deadifyAllScoped(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    DeadVisitor visitor (nodep, true, true, true, true);
    V3Global::dumpCheckGlobalTree("deadAllScoped.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}
