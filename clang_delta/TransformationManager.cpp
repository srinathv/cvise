//===----------------------------------------------------------------------===//
//
// Copyright (c) 2012 The University of Utah
// All rights reserved.
//
// This file is distributed under the University of Illinois Open Source
// License.  See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TransformationManager.h"

#include <sstream>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Parse/ParseAST.h"

#include "llvm/Config/config.h"

#include "Transformation.h"

using namespace clang;

TransformationManager* TransformationManager::Instance;

std::map<std::string, Transformation *> *
TransformationManager::TransformationsMapPtr;

TransformationManager *TransformationManager::GetInstance(void)
{
  if (TransformationManager::Instance)
    return TransformationManager::Instance;

  TransformationManager::Instance = new TransformationManager();
  assert(TransformationManager::Instance);

  TransformationManager::Instance->TransformationsMap = 
    *TransformationManager::TransformationsMapPtr;
  return TransformationManager::Instance;
}

bool TransformationManager::initializeCompilerInstance(std::string &ErrorMsg)
{
  if (ClangInstance) {
    ErrorMsg = "CompilerInstance has been initialized!";
    return false;
  }

  ClangInstance = new CompilerInstance();
  assert(ClangInstance);
  
  ClangInstance->createDiagnostics(0, NULL);
  ClangInstance->getLangOpts().C99 = 1;

  // Disable it for now: it causes some problems when building AST
  // for a function which has a non-declared callee, e.g., 
  // It results an empty AST for the caller. 
  // ClangInstance->getLangOpts().CPlusPlus = 1;
  TargetOptions &TargetOpts = ClangInstance->getTargetOpts();
  TargetOpts.Triple = LLVM_DEFAULT_TARGET_TRIPLE;
  TargetInfo *Target = 
    TargetInfo::CreateTargetInfo(ClangInstance->getDiagnostics(),
                                 TargetOpts);
  ClangInstance->setTarget(Target);
  ClangInstance->createFileManager();
  ClangInstance->createSourceManager(ClangInstance->getFileManager());
  ClangInstance->createPreprocessor();

  DiagnosticConsumer &DgClient = ClangInstance->getDiagnosticClient();
  DgClient.BeginSourceFile(ClangInstance->getLangOpts(),
                           &ClangInstance->getPreprocessor());
  ClangInstance->createASTContext();

  if (!ClangInstance->InitializeSourceManager(SrcFileName)) {
    ErrorMsg = "Cannot open source file!";
    return false;
  }

  return true;
}

void TransformationManager::Finalize(void)
{
  assert(TransformationManager::Instance);
  
  std::map<std::string, Transformation *>::iterator I, E;
  for (I = Instance->TransformationsMap.begin(), 
       E = Instance->TransformationsMap.end();
       I != E; ++I) {
    // CurrentTransformationImpl will be freed by ClangInstance
    if ((*I).second != Instance->CurrentTransformationImpl)
      delete (*I).second;
  }
  delete Instance->ClangInstance;

  delete Instance;
  Instance = NULL;
}

llvm::raw_ostream *TransformationManager::getOutStream(void)
{
  if (OutputFileName.empty())
    return &(llvm::outs());

  std::string Err;
  llvm::raw_fd_ostream *Out = 
    new llvm::raw_fd_ostream(OutputFileName.c_str(), Err);
  assert(Err.empty() && "Cannot open output file!");
  return Out;
}

void TransformationManager::closeOutStream(llvm::raw_ostream *OutStream)
{
  if (!OutputFileName.empty())
    delete OutStream;
}

bool TransformationManager::doTransformation(std::string &ErrorMsg)
{
  ErrorMsg = "";

  assert(CurrentTransformationImpl && "Bad transformation instance!");
  ClangInstance->setASTConsumer(CurrentTransformationImpl);
  ClangInstance->createSema(TU_Complete, 0);
  ClangInstance->getDiagnostics().setSuppressAllDiagnostics(true);

  CurrentTransformationImpl->setQueryInstanceFlag(QueryInstanceOnly);
  CurrentTransformationImpl->setTransformationCounter(TransformationCounter);

  ParseAST(ClangInstance->getSema());

  ClangInstance->getDiagnosticClient().EndSourceFile();

  if (QueryInstanceOnly) {
    return true;
  }

  llvm::raw_ostream *OutStream = getOutStream();
  bool RV;
  if (CurrentTransformationImpl->transSuccess()) {
    CurrentTransformationImpl->outputTransformedSource(*OutStream);
    RV = true;
  }
  else if (CurrentTransformationImpl->transInternalError()) {
    CurrentTransformationImpl->outputOriginalSource(*OutStream);
    RV = true;
  }
  else {
    CurrentTransformationImpl->getTransErrorMsg(ErrorMsg);
    RV = false;
  }
  closeOutStream(OutStream);
  return RV;
}

bool TransformationManager::verify(std::string &ErrorMsg)
{
  if (!CurrentTransformationImpl) {
    ErrorMsg = "Empty transformation instance!";
    return false;
  }

  if ((TransformationCounter <= 0) && 
      !CurrentTransformationImpl->skipCounter()) {
    ErrorMsg = "Invalid transformation counter!";
    return false;
  }

  return true;
}

void TransformationManager::registerTransformation(
       const char *TransName, 
       Transformation *TransImpl)
{
  if (!TransformationManager::TransformationsMapPtr) {
    TransformationManager::TransformationsMapPtr = 
      new std::map<std::string, Transformation *>();
  }

  assert((TransImpl != NULL) && "NULL Transformation!");
  assert((TransformationManager::TransformationsMapPtr->find(TransName) == 
          TransformationManager::TransformationsMapPtr->end()) &&
         "Duplicated transformation!");
  (*TransformationManager::TransformationsMapPtr)[TransName] = TransImpl;
}

void TransformationManager::printTransformations(void)
{
  llvm::outs() << "Registered Transformations:\n";

  std::map<std::string, Transformation *>::iterator I, E;
  for (I = TransformationsMap.begin(), 
       E = TransformationsMap.end();
       I != E; ++I) {
    llvm::outs() << "  [" << (*I).first << "]: "; 
    llvm::outs() << (*I).second->getDescription() << "\n";
  }
}

void TransformationManager::printTransformationNames(void)
{
  std::map<std::string, Transformation *>::iterator I, E;
  for (I = TransformationsMap.begin(), 
       E = TransformationsMap.end();
       I != E; ++I) {
    llvm::outs() << (*I).first << "\n";
  }
}

void TransformationManager::outputNumTransformationInstances(void)
{
  int NumInstances = 
    CurrentTransformationImpl->getNumTransformationInstances();
  llvm::outs() << "Available transformation instances: " 
               << NumInstances << "\n";
}

TransformationManager::TransformationManager(void)
  : CurrentTransformationImpl(NULL),
    TransformationCounter(-1),
    SrcFileName(""),
    OutputFileName(""),
    ClangInstance(NULL),
    QueryInstanceOnly(false)
{
  // Nothing to do
}

TransformationManager::~TransformationManager(void)
{
  if (!TransformationsMapPtr)
    delete TransformationsMapPtr;
}

