
#include "../../pch.h"

#include "mingw.h"
#include <assert.h>
#include "modulehandler.h"

using std::string;
using std::vector;
using std::set;
using std::map;

typedef set<string> set_string;
typedef map<string,Directory*> directory_map;

class Directory
{
public:
	string name;
	directory_map subdirs;
	Directory ( const string& name );
	void Add ( const char* subdir );
	void CreateRule ( FILE* f, const string& parent );
};

Directory::Directory ( const string& name_ )
	: name(name_)
{
}

void Directory::Add ( const char* subdir )
{
	const char* p = strpbrk ( subdir, "/\\" );
	if ( !p )
		p = subdir + strlen(subdir);
	string s ( subdir, p-subdir );
	if ( subdirs.find(s) == subdirs.end() )
		subdirs[s] = new Directory(s);
	if ( *p && *++p )
		subdirs[s]->Add ( p );
}

void
Directory::CreateRule ( FILE* f, const string& parent )
{
	string path;

	if ( parent.size() )
	{
		fprintf ( f,
			"%s%c%s: %s\n",
			parent.c_str (),
			CSEP,
			name.c_str (),
			parent.c_str () );

		fprintf ( f,
			"\t$(ECHO_MKDIR)\n" );

		fprintf ( f,
			"\t${mkdir} $@\n" );

		path = parent + SSEP + name;
	}
	else
		path = name;

	for ( directory_map::iterator i = subdirs.begin();
		i != subdirs.end();
		++i )
	{
		i->second->CreateRule ( f, path );
	}
}

static class MingwFactory : public Backend::Factory
{
public:
	MingwFactory() : Factory ( "mingw" ) {}
	Backend* operator() ( Project& project )
	{
		return new MingwBackend ( project );
	}
} factory;


MingwBackend::MingwBackend ( Project& project )
	: Backend ( project ),
	  int_directories ( new Directory("$(INTERMEDIATE)") ),
	  out_directories ( new Directory("$(OUTPUT)") )
{
}

MingwBackend::~MingwBackend()
{
	delete int_directories;
	delete out_directories;
}

string
MingwBackend::AddDirectoryTarget ( const string& directory, bool out )
{
	const char* dir_name = "$(INTERMEDIATE)";
	Directory* dir = int_directories;
	if ( out )
	{
		dir_name = "$(OUTPUT)";
		dir = out_directories;
	}
	dir->Add ( directory.c_str() );
	return dir_name;
}

void
MingwBackend::Process ()
{
	size_t i;

	DetectPCHSupport();

	CreateMakefile ();
	GenerateHeader ();
	GenerateGlobalVariables ();
	GenerateXmlBuildFilesMacro();

	vector<MingwModuleHandler*> v;

	for ( i = 0; i < ProjectNode.modules.size (); i++ )
	{
		Module& module = *ProjectNode.modules[i];
		MingwModuleHandler* h = MingwModuleHandler::InstanciateHandler (
			module,
			this );
		if ( module.host == HostDefault )
		{
			module.host = h->DefaultHost();
			assert ( module.host != HostDefault );
		}
		v.push_back ( h );
	}

	size_t iend = v.size ();

	for ( i = 0; i < iend; i++ )
		v[i]->GenerateObjectMacro();
	fprintf ( fMakefile, "\n" );
	for ( i = 0; i < iend; i++ )
		v[i]->GenerateTargetMacro();
	fprintf ( fMakefile, "\n" );

	GenerateAllTarget ( v );
	GenerateInitTarget ();

	for ( i = 0; i < iend; i++ )
		v[i]->GenerateOtherMacros();

	for ( i = 0; i < iend; i++ )
	{
		MingwModuleHandler& h = *v[i];
		h.GeneratePreconditionDependencies ();
		h.Process ();
		h.GenerateInvocations ();
		h.GenerateCleanTarget ();
		delete v[i];
	}

	GenerateDirectoryTargets ();
	CheckAutomaticDependencies ();
	CloseMakefile ();
}

void
MingwBackend::CreateMakefile ()
{
	fMakefile = fopen ( ProjectNode.makefile.c_str (), "w" );
	if ( !fMakefile )
		throw AccessDeniedException ( ProjectNode.makefile );
	MingwModuleHandler::SetBackend ( this );
	MingwModuleHandler::SetMakefile ( fMakefile );
	MingwModuleHandler::SetUsePch ( use_pch );
}

void
MingwBackend::CloseMakefile () const
{
	if (fMakefile)
		fclose ( fMakefile );
}

void
MingwBackend::GenerateHeader () const
{
	fprintf ( fMakefile, "# THIS FILE IS AUTOMATICALLY GENERATED, EDIT 'ReactOS.xml' INSTEAD\n\n" );
}

void
MingwBackend::GenerateProjectCFlagsMacro ( const char* assignmentOperation,
                                           IfableData& data ) const
{
	size_t i;

	fprintf (
		fMakefile,
		"PROJECT_CFLAGS %s",
		assignmentOperation );
	for ( i = 0; i < data.includes.size(); i++ )
	{
		fprintf (
			fMakefile,
			" -I%s",
			data.includes[i]->directory.c_str() );
	}

	for ( i = 0; i < data.defines.size(); i++ )
	{
		Define& d = *data.defines[i];
		fprintf (
			fMakefile,
			" -D%s",
			d.name.c_str() );
		if ( d.value.size() )
			fprintf (
				fMakefile,
				"=%s",
				d.value.c_str() );
	}
	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::GenerateGlobalCFlagsAndProperties (
	const char* assignmentOperation,
	IfableData& data ) const
{
	size_t i;

	for ( i = 0; i < data.properties.size(); i++ )
	{
		Property& prop = *data.properties[i];
		fprintf ( fMakefile, "%s := %s\n",
			prop.name.c_str(),
			prop.value.c_str() );
	}

	if ( data.includes.size() || data.defines.size() )
	{
		GenerateProjectCFlagsMacro ( assignmentOperation,
		                             data );
	}

	for ( i = 0; i < data.ifs.size(); i++ )
	{
		If& rIf = *data.ifs[i];
		if ( rIf.data.defines.size()
			|| rIf.data.includes.size()
			|| rIf.data.ifs.size() )
		{
			fprintf (
				fMakefile,
				"ifeq (\"$(%s)\",\"%s\")\n",
				rIf.property.c_str(),
				rIf.value.c_str() );
			GenerateGlobalCFlagsAndProperties (
				"+=",
				rIf.data );
			fprintf (
				fMakefile,
				"endif\n\n" );
		}
	}
}

string
MingwBackend::GenerateProjectLFLAGS () const
{
	string lflags;
	for ( size_t i = 0; i < ProjectNode.linkerFlags.size (); i++ )
	{
		LinkerFlag& linkerFlag = *ProjectNode.linkerFlags[i];
		if ( lflags.length () > 0 )
			lflags += " ";
		lflags += linkerFlag.flag;
	}
	return lflags;
}

void
MingwBackend::GenerateGlobalVariables () const
{
	GenerateGlobalCFlagsAndProperties (
		"=",
		ProjectNode.non_if_data );
	fprintf ( fMakefile, "PROJECT_RCFLAGS = $(PROJECT_CFLAGS)\n" );
	fprintf ( fMakefile, "PROJECT_LFLAGS = %s\n",
	          GenerateProjectLFLAGS ().c_str () );
	fprintf ( fMakefile, "\n" );
}

bool
MingwBackend::IncludeInAllTarget ( const Module& module ) const
{
	if ( module.type == ObjectLibrary )
		return false;
	if ( module.type == BootSector )
		return false;
	if ( module.type == Iso )
		return false;
	return true;
}

void
MingwBackend::GenerateAllTarget ( const vector<MingwModuleHandler*>& handlers ) const
{
	fprintf ( fMakefile, "all:" );
	int wrap_count = 0;
	size_t iend = handlers.size ();
	for ( size_t i = 0; i < iend; i++ )
	{
		const Module& module = handlers[i]->module;
		if ( IncludeInAllTarget ( module ) )
		{
			if ( wrap_count++ == 5 )
				fprintf ( fMakefile, " \\\n\t\t" ), wrap_count = 0;
			fprintf ( fMakefile,
			          " %s",
			          GetTargetMacro(module).c_str () );
		}
	}
	fprintf ( fMakefile, "\n\t\n\n" );
}

string
MingwBackend::GetBuildToolDependencies () const
{
	string dependencies;
	for ( size_t i = 0; i < ProjectNode.modules.size (); i++ )
	{
		Module& module = *ProjectNode.modules[i];
		if ( module.type == BuildTool )
		{
			if ( dependencies.length () > 0 )
				dependencies += " ";
			dependencies += module.GetDependencyPath ();
		}
	}
	return dependencies;
}

void
MingwBackend::GenerateInitTarget () const
{
	fprintf ( fMakefile,
	          "INIT = %s\n",
	          GetBuildToolDependencies ().c_str () );
	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::GenerateXmlBuildFilesMacro() const
{
	fprintf ( fMakefile,
	          "XMLBUILDFILES = %s \\\n",
	          ProjectNode.GetProjectFilename ().c_str () );
	string xmlbuildFilenames;
	int numberOfExistingFiles = 0;
	for ( size_t i = 0; i < ProjectNode.xmlbuildfiles.size (); i++ )
	{
		XMLInclude& xmlbuildfile = *ProjectNode.xmlbuildfiles[i];
		if ( !xmlbuildfile.fileExists )
			continue;
		numberOfExistingFiles++;
		if ( xmlbuildFilenames.length () > 0 )
			xmlbuildFilenames += " ";
		xmlbuildFilenames += NormalizeFilename ( xmlbuildfile.topIncludeFilename );
		if ( numberOfExistingFiles % 5 == 4 || i == ProjectNode.xmlbuildfiles.size () - 1 )
		{
			fprintf ( fMakefile,
			          "\t%s",
			          xmlbuildFilenames.c_str ());
			if ( i == ProjectNode.xmlbuildfiles.size () - 1 )
			{
				fprintf ( fMakefile, "\n" );
			}
			else
			{
				fprintf ( fMakefile,
				          " \\\n",
				          xmlbuildFilenames.c_str () );
			}
			xmlbuildFilenames.resize ( 0 );
		}
		numberOfExistingFiles++;
	}
	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::CheckAutomaticDependencies ()
{
	AutomaticDependency automaticDependency ( ProjectNode );
	automaticDependency.Process ();
	automaticDependency.CheckAutomaticDependencies ();
}

bool
MingwBackend::IncludeDirectoryTarget ( const string& directory ) const
{
	if ( directory == "$(INTERMEDIATE)" SSEP "tools")
		return false;
	else
		return true;
}

void
MingwBackend::GenerateDirectoryTargets ()
{
	// TODO FIXME - write new directory creation
	for ( int i = 0; i < 2; i++ )
	{
		Directory& d = *(!i ? int_directories : out_directories);
		if ( i ) fprintf ( fMakefile, "ifneq ($(INTERMEDIATE),$(OUTPUT))\n" );
		d.CreateRule ( fMakefile, "" );
		if ( i ) fprintf ( fMakefile, "endif\n" );
	}
}

string
FixupTargetFilename ( const string& targetFilename )
{
	return NormalizeFilename ( targetFilename );
}

void
MingwBackend::DetectPCHSupport()
{
#ifdef WIN32
	string sNUL = "NUL";
#else
	string sNUL = "/dev/null";
#endif
	string path = "tools" SSEP "rbuild" SSEP "backend" SSEP "mingw" SSEP "pch_detection.h";
	string cmd = ssprintf(
		"gcc -c %s 2>%s",
		path.c_str (),
		sNUL.c_str () );
	system ( cmd.c_str() );
	path += ".gch";

	FILE* f = fopen ( path.c_str(), "rb" );
	if ( f )
	{
		use_pch = true;
		fclose(f);
		unlink ( path.c_str() );
	}
	else
		use_pch = false;

	// TODO FIXME - eventually check for ROS_USE_PCH env var and
	// allow that to override use_pch if true
}
