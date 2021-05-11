#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include "boost/range.hpp"
#include <set>

namespace crab_llvm
{
  using namespace llvm;

  class LowerCstExpr: public ModulePass 
  {
    
    ConstantExpr* hasCstExpr(Value *V, std::set<Value*> &visited)
    {
      if (visited.count (V) > 0) return nullptr;
      
      visited.insert (V);
      if (Constant * cst = dyn_cast<Constant>(V))
      {
        if (ConstantExpr * ce = dyn_cast<ConstantExpr>(cst)) 
          return ce;
        else
        {
          // for ConstantStruct, ConstantArray, etc, we need to check
          // recursively.
          for (unsigned u=0; u < cst->getNumOperands (); ++u)
          {
            Use& p = cst->getOperandUse(u);
            // for (auto p : boost::make_iterator_range (cst->op_begin (),
            //                                           cst->op_end ()))
            // {
            if (ConstantExpr * cst_exp_i = hasCstExpr(p.get (), visited))
              return cst_exp_i;
          }
        }          
      }
      return nullptr;
    }
    
    ConstantExpr* hasCstExpr(Value *V)
    {
      std::set<Value*> visited;
      return hasCstExpr(V,visited);
    }
    
    Instruction * lowerCstExpr(ConstantExpr* CstExp, 
                               Instruction* InsertionLoc) 
    {
    
      assert(CstExp);
      
      Instruction * NewInst = nullptr;
      switch (CstExp->getOpcode()) 
      {
        case Instruction::Add:
        case Instruction::FAdd:  
        case Instruction::Sub:
        case Instruction::FSub:
        case Instruction::Mul:
        case Instruction::FMul:
        case Instruction::UDiv:
        case Instruction::SDiv:
        case Instruction::FDiv:
        case Instruction::URem:
        case Instruction::SRem:
        case Instruction::FRem:
        case Instruction::Shl:
        case Instruction::LShr:
        case Instruction::AShr:
        case Instruction::And:
        case Instruction::Or:
        case Instruction::Xor: 
          {
            Instruction::BinaryOps BinOp = 
                (Instruction::BinaryOps)(CstExp->getOpcode());
            NewInst = BinaryOperator::Create(BinOp,
                                         CstExp->getOperand(0),
                                             CstExp->getOperand(1),
                                             CstExp->getName(),
                                             InsertionLoc);  // insert before
          }
          break;
        case Instruction::Trunc:
        case Instruction::ZExt:
        case Instruction::SExt:
        case Instruction::FPToUI:
        case Instruction::FPToSI:
        case Instruction::UIToFP:
        case Instruction::SIToFP:
        case Instruction::FPTrunc:
        case Instruction::FPExt:
        case Instruction::PtrToInt:
        case Instruction::IntToPtr:
        case Instruction::BitCast: 
          {
            Instruction::CastOps CastOp = (Instruction::CastOps)(CstExp->getOpcode());
            NewInst = CastInst::Create(CastOp,
                                       CstExp->getOperand(0),
                                       CstExp->getType(),
                                       CstExp->getName(),
                                       InsertionLoc); // insert before
          }
          break;
        case Instruction:: FCmp:
        case Instruction:: ICmp: 
        {
          Instruction::OtherOps OtherOp = (Instruction::OtherOps)(CstExp->getOpcode());
          NewInst = CmpInst::Create(OtherOp,
                                    CstExp->getPredicate(),
                                    CstExp->getOperand(0),
                                    CstExp->getOperand(1),
                                    CstExp->getName(),
                                    InsertionLoc);
          break;
        }
        case Instruction:: Select:
          NewInst = SelectInst::Create(CstExp->getOperand(0),
                                       CstExp->getOperand(1),
                                       CstExp->getOperand(2),
                                       CstExp->getName(),
                                       InsertionLoc);
          break;
        case Instruction::GetElementPtr: 
          {
            SmallVector<Value *, 4>  VIdxs;
            for (unsigned i = 1; i < CstExp->getNumOperands(); i++)
              VIdxs.push_back(CstExp->getOperand(i));
            
            ArrayRef<Value*> Idxs(VIdxs); 
            NewInst = (GetElementPtrInst::Create(CstExp->getOperand(0),
                                                 Idxs,
                                                 CstExp->getName(),
                                                 InsertionLoc));
            
          }
          break;
        default: 
          // CallInst, VAArg, ExtractElement, InserElement, 
          // ShuffleElement, ExtractValue, InsertValue
          assert(false && "Unhandled constant expression!\n");
          break;
      }
      assert(NewInst);
      return NewInst;
    }
    
    bool runOnFunction(Function & F) 
    {
      
      SmallPtrSet<Instruction*, 8> worklist;
      
      for (inst_iterator It = inst_begin(F), E = inst_end(F); It != E; ++It)
      {
        Instruction *I = &*It;
        
        for (unsigned int i=0; i < I->getNumOperands(); ++i) 
        {
          if (hasCstExpr (I->getOperand(i))) 
            worklist.insert (I);
        }
      }
      
      
      bool change = !worklist.empty ();
      
      while (!worklist.empty()) 
      {
        auto It = worklist.begin ();
        Instruction*I = *It;
        worklist.erase (*It);
        
        if (PHINode * PHI = dyn_cast<PHINode>(I)) 
        {
          for (unsigned int i = 0; i < PHI->getNumIncomingValues (); ++i) 
          {
            Instruction* InsertLoc = PHI->getIncomingBlock (i)->getTerminator ();        
            assert(InsertLoc);
            
            if (ConstantExpr * CstExp = hasCstExpr (PHI->getIncomingValue(i))) 
            {
              Instruction* NewInst = lowerCstExpr (CstExp, InsertLoc);
              for (unsigned int j=i; j < PHI->getNumIncomingValues(); j++) 
              {
                if ( (PHI->getIncomingValue (j) == PHI->getIncomingValue (i)) &&
                     (PHI->getIncomingBlock (j) == PHI->getIncomingBlock (i))) 
                {
                  PHI->setIncomingValue (j, NewInst);
                }
              }
              worklist.insert (NewInst);
            }
          }
        } 
        else 
        {
          for (unsigned int i=0; i < I->getNumOperands (); ++i) 
          {
            if (ConstantExpr* CstExp = hasCstExpr (I->getOperand(i))) 
            {
              Instruction * NewInst = lowerCstExpr (CstExp, I);
              I->replaceUsesOfWith (CstExp, NewInst);
              worklist.insert (NewInst);
            }
          }
        }
      }
      return change;
    }
    
   public:
    
    static char ID; 
    
    LowerCstExpr(): ModulePass (ID) {  }
    
    virtual bool runOnModule(Module &M) {
     bool change = false;
     for (auto &F: M){ change |= runOnFunction(F); }
     return change;
    }
    
    void getAnalysisUsage (AnalysisUsage &AU) const 
    {AU.setPreservesAll ();}

    virtual const char * getPassName() const {
      return "Lower constant expressions";
    }
    
  };

  char LowerCstExpr::ID = 0;
  Pass* createLowerCstExprPass () { return new LowerCstExpr (); }

} // end namespace 
