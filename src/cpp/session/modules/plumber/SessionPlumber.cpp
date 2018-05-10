/*
 * SessionPlumber.cpp
 *
 * Copyright (C) 2009-18 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionPlumber.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <core/Algorithm.hpp>
#include <core/Error.hpp>
#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/YamlUtil.hpp>

#include <r/RExec.hpp>
#include <r/RRoutines.hpp>

#include <session/SessionRUtil.hpp>
#include <session/SessionOptions.hpp>
#include <session/SessionModuleContext.hpp>

#define kPlumberTypeDocument "plumber-file"
#define kPlumberTypeEntrypoint "plumber-entrypoint"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules { 
namespace plumber {

namespace {

PlumberFileType getPlumberFileType(const FilePath& filePath, const std::string& contents)
{
   // filename "entrypoint.R" has special meaning when running locally or publishing to rsConnect
   if (filePath.stem() == "entrypoint")
   {
      return PlumberFileType::PlumberEntrypoint; 
   }
   
   // If there's an annotation for a plumber filter, API endpoint, or asset, we declare
   // this to be a plumber file. We don't care about details or fully checking syntax, just enough
   // to enable plumber-specific functionality.
   static const boost::regex rePlumberAnnotation(
         R"(^#['\*]\s*@(get|put|post|filter|assets|use|delete|head|options|patch)\s)");
   return regex_utils::search(contents, rePlumberAnnotation) ? 
          PlumberFileType::PlumberFile : PlumberFileType::PlumberNone;
}

std::string onDetectPlumberSourceType(boost::shared_ptr<source_database::SourceDocument> pDoc)
{
   if (!pDoc->path().empty() && pDoc->isRFile())
   {
      FilePath filePath = module_context::resolveAliasedPath(pDoc->path());
      PlumberFileType type = getPlumberFileType(filePath, pDoc->contents());
      switch(type)
      {
         case PlumberFileType::PlumberNone:
            return std::string();
         case PlumberFileType::PlumberFile:
            return kPlumberTypeDocument;
         case PlumberFileType::PlumberEntrypoint:
            return kPlumberTypeEntrypoint;
      }
   }
   return std::string();
}

Error getPlumberCapabilities(const json::JsonRpcRequest& request, json::JsonRpcResponse* pResponse)
{
   json::Object capsJson;
   capsJson["installed"] = module_context::isPackageInstalled("plumber");
   pResponse->setResult(capsJson);

   return Success();
}

FilePath plumberTemplatePath(const std::string& name)
{
   return session::options().rResourcesPath().childPath("templates/plumber/" + name);
}

Error copyTemplateFile(const std::string& templateFileName, const FilePath& target)
{
   FilePath templatePath = plumberTemplatePath(templateFileName);
   Error error = templatePath.copy(target);
   if (!error)
   {
      // account for existing permissions on source template file
      module_context::events().onPermissionsChanged(target);
   }
   return error;
}

Error createPlumberAPI(const json::JsonRpcRequest& request, json::JsonRpcResponse* pResponse)
{
   json::Array result;
   
   std::string apiName;
   std::string apiDirString;
   
   Error error = json::readParams(request.params,
                                  &apiName,
                                  &apiDirString);
   if (error)
   {
      LOG_ERROR(error);
      return error;
   }
   
   FilePath apiDir = module_context::resolveAliasedPath(apiDirString);
   FilePath plumberDir = apiDir.complete(apiName);
   
   // if plumberDir exists and is not an empty directory, bail
   if (plumberDir.exists())
   {
      if (!plumberDir.isDirectory())
      {
         pResponse->setError(
                  fileExistsError(ERROR_LOCATION),
                  "The directory '" + module_context::createAliasedPath(plumberDir) + "' already exists "
                  "and is not a directory");
         return Success();
      }
      
      std::vector<FilePath> children;
      error = plumberDir.children(&children);
      if (error)
         LOG_ERROR(error);
      
      if (!children.empty())
      {
         pResponse->setError(
                  fileExistsError(ERROR_LOCATION),
                  "The directory '" + module_context::createAliasedPath(plumberDir) + "' already exists "
                  "and is not empty");
         return Success();
      }
   }
   else
   {
      error = plumberDir.ensureDirectory();
      if (error)
      {
         pResponse->setError(error);
         return Success();
      }
   }
   
   const std::string templateFile = "plumber.R";
   
   // if file already exists, report that as an error
   FilePath target = plumberDir.complete(templateFile);
   std::string aliasedPath = module_context::createAliasedPath(plumberDir.complete(templateFile));
   result.push_back(aliasedPath);
   if (target.exists())
   {
      std::string message = "The file '" + aliasedPath + "' already exists";
      pResponse->setError(
               fileExistsError(ERROR_LOCATION),
               message);
      return Success();
   }
   
   // copy the file
   error = copyTemplateFile(templateFile, target);
   if (error)
   {
      pResponse->setError(error, "Failed to write '" + module_context::createAliasedPath(target) + "'");
      return Success();
   }
   
   pResponse->setResult(result);
   return Success();
}

} // anonymous namespace

PlumberFileType plumberTypeFromExtendedType(const std::string& extendedType)
{
   if (extendedType == kPlumberTypeDocument)
      return PlumberFileType::PlumberFile;
   else if (extendedType == kPlumberTypeEntrypoint)
      return PlumberFileType::PlumberEntrypoint;
   return PlumberFileType::PlumberNone;
}

Error initialize()
{
   using namespace module_context;
   using boost::bind;
   
   events().onDetectSourceExtendedType.connect(onDetectPlumberSourceType);
   
   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "get_plumber_capabilities", getPlumberCapabilities))
      (bind(registerRpcMethod, "create_plumber_api", createPlumberAPI));

   return initBlock.execute();
}

} // namespace plumber
} // namespace modules
} // namespace session
} // namespace rstudio

