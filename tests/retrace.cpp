// ======================================================================== //
// Copyright 2009-2014 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "sys/platform.h"
#include "sys/ref.h"
#include "embree2/rtcore.h"
#include "embree2/rtcore_ray.h"
#include "../kernels/common/default.h"
#include "../kernels/common/raystream_log.h"
#include <vector>
#include <iostream>
#include <fstream>

#define DBG(x) 

namespace embree
{
  struct RayStreamStats
  {
    size_t numTotalRays;
    size_t numRayPackets;
    size_t numIntersectRayPackets;
    size_t numOccludedRayPackets;
    size_t numIntersectRays;
    size_t numOccludedRays;
    size_t num4widePackets;
    size_t num8widePackets;

    RayStreamStats() {
      memset(this,0,sizeof(RayStreamStats));
    }

    void add(RayStreamLogger::LogRay16 &r)
    {
#if defined(__MIC__)
      size_t numRays = countbits((mic_m)r.m_valid);
      numRayPackets++;
      numTotalRays += numRays;
      if (r.type == RayStreamLogger::RAY_INTERSECT)
	{
	  numIntersectRayPackets++;
	  numIntersectRays += numRays;
	}
      else if (r.type == RayStreamLogger::RAY_OCCLUDED)
	{
	  numOccludedRayPackets++;
	  numOccludedRays += numRays;
	}
      else
	FATAL("unknown log ray type");
      num4widePackets += (numRays+3)/4;
      num8widePackets += (numRays+7)/8;
#endif
    }

  };

  __forceinline std::ostream &operator<<(std::ostream &o, const RayStreamStats &s)
    {
      o << "numTotalRays                      = " << s.numTotalRays << std::endl;
      o << "numRayPackets                     = " << s.numRayPackets << std::endl;
      o << "numIntersectionRays               = " << s.numIntersectRays << " [" << 100. * (double)s.numIntersectRays / s.numTotalRays << "%]" << std::endl;
      o << "numOcclusionRays                  = " << s.numOccludedRays << " [" << 100. * (double)s.numOccludedRays / s.numTotalRays << "%]" << std::endl;
      o << "avg. intersect packet utilization = " << 100. *  (double)s.numIntersectRays / (s.numIntersectRayPackets * 16.) << "%" << std::endl;
      o << "avg. occluded  packet utilization = " << 100. *  (double)s.numOccludedRays  / (s.numOccludedRayPackets  * 16.) << "%" << std::endl;
      o << "avg. total packet utilization     = " << 100. * (double)s.numTotalRays / (s.numRayPackets * 16.)  << "%" << std::endl;
      o << "avg. 4-wide packet utilization    = " << 100. * (double)s.numTotalRays / (s.num4widePackets * 4.)  << "%" << std::endl;
      o << "avg. 8-wide packet utilization    = " << 100. * (double)s.numTotalRays / (s.num8widePackets * 8.)  << "%" << std::endl;

      return o;
    } 
  

#if !defined(__MIC__)
  RTCAlgorithmFlags aflags = (RTCAlgorithmFlags) (RTC_INTERSECT1 | RTC_INTERSECT4 | RTC_INTERSECT8);
#else
  RTCAlgorithmFlags aflags = (RTCAlgorithmFlags) (RTC_INTERSECT1 | RTC_INTERSECT16);
#endif
  /* configuration */
  static std::string g_rtcore = "verbose=2,traverser=single";

  /* vertex and triangle layout */
  struct Vertex   { float x,y,z,a; };
  struct Triangle { int v0, v1, v2; };

  static AtomicCounter g_counter = 0;
  static bool g_check = false;
  static size_t g_numThreads = 4;
  static size_t g_frames = 4;
  static AtomicCounter g_rays_traced = 0;

#if defined(__MIC__)
  static std::string g_binaries_path = "/home/micuser/";
#else
  static std::string g_binaries_path = "./";
#endif


#define AssertNoError() \
  if (rtcGetError() != RTC_NO_ERROR) return false;
#define AssertAnyError() \
  if (rtcGetError() == RTC_NO_ERROR) return false;
#define AssertError(code) \
  if (rtcGetError() != code) return false;

  static void parseCommandLine(int argc, char** argv)
  {
    for (int i=1; i<argc; i++)
    {
      std::string tag = argv[i];
      if (tag == "") return;

      else if (tag == "-rtcore" && i+1<argc) {
        g_rtcore += std::stringOf(',') + argv[++i];
      }
      /* rtcore configuration */
      else if (tag == "-check") {
        g_check = true;
      }      
      else if (tag == "-threads" && i+1<argc) {
        g_numThreads = atoi(argv[++i]);
      }
      else if (tag == "-frames" && i+1<argc) {
        g_frames = atoi(argv[++i]);
      }

      /* skip unknown command line parameter */
      else {

	g_binaries_path = tag;
        // std::cerr << "unknown command line parameter: " << tag << " ";
        // std::cerr << std::std::endl;
      }
    }
  }


  void *loadGeometryData(std::string &geometryFile)
  {
    std::ifstream geometryData;

    geometryData.open(geometryFile.c_str(),std::ios::in | std::ios::binary);
    if (!geometryData) { FATAL("could not open geometry data file"); }
    geometryData.seekg(0, std::ios::beg);
    std::streampos begin = geometryData.tellg();
    geometryData.seekg(0, std::ios::end);
    std::streampos end = geometryData.tellg();
    size_t fileSize = end - begin;
    char *ptr = (char*)os_malloc(fileSize);
    geometryData.seekg(0, std::ios::beg);
    geometryData.read(ptr,fileSize);
    return ptr;
  }

  void *loadRayStreamData(std::string &rayStreamFile, size_t &numLogRayStreamElements)
  {
    std::ifstream rayStreamData;

    rayStreamData.open(rayStreamFile.c_str(),std::ios::in | std::ios::binary);
    if (!rayStreamData) { FATAL("could not open raystream data file"); }
    rayStreamData.seekg(0, std::ios::beg);
    std::streampos begin = rayStreamData.tellg();
    rayStreamData.seekg(0, std::ios::end);
    std::streampos end = rayStreamData.tellg();
    size_t fileSize = end - begin;
    char *ptr = (char*)os_malloc(fileSize);
    rayStreamData.seekg(0, std::ios::beg);
    rayStreamData.read(ptr,fileSize);
    numLogRayStreamElements = fileSize / sizeof(RayStreamLogger::LogRay16);
    return ptr;
  }

  RayStreamStats analyseRayStreamData(RayStreamLogger::LogRay16 *r, size_t numLogRayStreamElements)
  {
    RayStreamStats stats;
    std::cout << "numLogRayStreamElements " << numLogRayStreamElements << std::endl;
    for (size_t i=0;i<numLogRayStreamElements;i++)
      stats.add(r[i]);
    return stats;
  }

  RTCScene transferGeometryData(char *g)
  {
    RTCScene scene = rtcNewScene(RTC_SCENE_STATIC,aflags);

    size_t numGroups = *(size_t*)g;
    g += sizeof(size_t);
    size_t numTotalTriangles = *(size_t*)g;
    g += sizeof(size_t);
    DBG_PRINT(numGroups);
    DBG_PRINT(numTotalTriangles);

    for (size_t i=0; i<numGroups; i++) 
      {
	size_t numVertices = *(size_t*)g;
	g += sizeof(size_t);
	size_t numTriangles = *(size_t*)g;
	g += sizeof(size_t);
	Vertex *vtx = (Vertex*)g;
	g += sizeof(Vertex)*numVertices;
	Triangle *tri = (Triangle *)g;
	g += sizeof(Triangle)*numTriangles;
	if (((size_t)g % 16) != 0)
	  g += 16 - ((size_t)g % 16);

	if ((size_t)vtx % 16 != 0)
	  FATAL("vtx array alignment");

	unsigned int geometry = rtcNewTriangleMesh (scene, RTC_GEOMETRY_STATIC, numTriangles, numVertices);
	rtcSetBuffer(scene, geometry, RTC_VERTEX_BUFFER, vtx, 0, sizeof(Vec3fa      ));
	rtcSetBuffer(scene, geometry, RTC_INDEX_BUFFER,  tri, 0, sizeof(Triangle));
      }

    rtcCommit(scene);
    return scene;
  }

  size_t check_ray16_packets(const size_t index, const unsigned int i_valid, RTCRay16 &start, RTCRay16 &end)
  {
#if defined(__MIC__)
    const mic_m m_valid = (mic_m)i_valid;
    mic_i start_primID = load16i(start.primID);
    mic_i end_primID   = load16i(end.primID);

    mic_i start_geomID = load16i(start.geomID);
    mic_i end_geomID   = load16i(end.geomID);

    mic_f start_u = load16f(start.u);
    mic_f end_u   = load16f(end.u);

    mic_f start_v = load16f(start.v);
    mic_f end_v   = load16f(end.v);

    mic_f start_t = load16f(start.tfar);
    mic_f end_t   = load16f(end.tfar);

    const mic_m m_primID = eq(m_valid,start_primID,end_primID);
    const mic_m m_geomID = eq(m_valid,start_geomID,end_geomID);
    const mic_m m_u      = eq(m_valid,start_u,end_u);
    const mic_m m_v      = eq(m_valid,start_v,end_v);
    const mic_m m_t      = eq(m_valid,start_t,end_t);

    if ( m_primID != m_valid )
      {
	DBG(
	    DBG_PRINT(index);
	    DBG_PRINT(m_valid);
	    DBG_PRINT(m_primID);
	    DBG_PRINT(start_primID);
	    DBG_PRINT(end_primID);
	    );
	return countbits(m_primID^m_valid);
      }

    if ( m_geomID != m_valid )
      {
	DBG(
	    DBG_PRINT( index );
	    DBG_PRINT( m_valid );
	    DBG_PRINT( m_geomID );
	    DBG_PRINT( start_geomID );
	    DBG_PRINT( end_geomID );
	    );
	return countbits(m_geomID^m_valid);
      }

    if ( m_u != m_valid )
      {
	DBG(
	    DBG_PRINT( index );
	    DBG_PRINT( m_valid );
	    DBG_PRINT( m_u );
	    DBG_PRINT( start_u );
	    DBG_PRINT( end_u );
	    );
	return countbits(m_u^m_valid);
      }

    if ( m_v != m_valid )
      {
	DBG(
	    DBG_PRINT( index );
	    DBG_PRINT( m_valid );
	    DBG_PRINT( m_v );
	    DBG_PRINT( start_v );
	    DBG_PRINT( end_v );
	    );
	return countbits(m_v^m_valid);
      }

    if ( m_t != m_valid )
      {
	DBG(
	    DBG_PRINT( index );
	    DBG_PRINT( m_valid );
	    DBG_PRINT( m_t );
	    DBG_PRINT( start_t );
	    DBG_PRINT( end_t );
	    );
	return countbits(m_t^m_valid);
      }
#endif
    return 0;
  }

#define RAY_BLOCK_SIZE 1

  void retrace_loop(RTCScene scene, 
		    RayStreamLogger::LogRay16 *r, 
		    RayStreamLogger::LogRay16 *verify, 
		    size_t numLogRayStreamElements, 
		    size_t threadID,
		    size_t numThreads,
		    bool check)
  {
    size_t rays = 0;
    size_t diff = 0;
    while(1)
      {
	size_t global_index = g_counter.add(RAY_BLOCK_SIZE);
	if (global_index >= numLogRayStreamElements) break;
	size_t startID = global_index;
	size_t endID   = min(numLogRayStreamElements,startID+RAY_BLOCK_SIZE);

	for (size_t index=startID;index<endID;index++)
	  {
#if defined(__MIC__)
	    RTCRay16 &ray16 = r[index].ray16;
	    mic_i valid = select((mic_m)r[index].m_valid,mic_i(-1),mic_i(0));
	    rays += countbits( (mic_m)r[index].m_valid );

	    r[index+1].prefetchL2();

	    if (r[index].type == RayStreamLogger::RAY_INTERSECT)
	      rtcIntersect16(&valid,scene,ray16);
	    else 
	      rtcOccluded16(&valid,scene,ray16);
	    r[index].evict();

	    if (unlikely(check))
	      diff += check_ray16_packets(index, r[index].m_valid,  r[index].ray16, verify[index].ray16);
#endif
	  }
      }
    if (unlikely(check && diff))
      {
	DBG_PRINT(diff);
	DBG_PRINT(100. * diff / rays);
      }
    g_rays_traced.add(rays);
  }

  struct RetraceTask
  {
    RTCScene scene;
    RayStreamLogger::LogRay16 *r;
    RayStreamLogger::LogRay16 *verify;
    size_t numLogRayStreamElements;
    size_t threads;
    bool check;
  };


  void retrace_loop_parallel(RetraceTask* task, size_t threadIndex, size_t threadCount, size_t taskIndex, size_t taskCount, TaskScheduler::Event* event)
  {
    retrace_loop(task->scene,task->r,task->verify,task->numLogRayStreamElements,threadIndex,threadCount,task->check);
  }

  void launch_retrace_loop(RTCScene scene, 
			   RayStreamLogger::LogRay16 *r, 
			   RayStreamLogger::LogRay16 *verify, 
			   size_t numLogRayStreamElements, 
			   bool check , 
			   size_t threads )
  {
    RetraceTask rt;
    rt.scene = scene;
    rt.r = r;
    rt.verify = verify;
    rt.numLogRayStreamElements = numLogRayStreamElements;
    rt.threads = threads;
    rt.check = check;
    
    embree::TaskScheduler::EventSync event;
    embree::TaskScheduler::Task task(&event,(TaskScheduler::runFunction)retrace_loop_parallel,&rt,g_numThreads,NULL,NULL,"retrace");
    embree::TaskScheduler::addTask(-1,TaskScheduler::GLOBAL_FRONT,&task);
    event.sync();
  }

  /* main function in embree namespace */
  int main(int argc, char** argv) 
  {
    setAffinity(0);
    /* parse command line */  
    parseCommandLine(argc,argv);

    DBG_PRINT( g_binaries_path );

    /* perform tests */
    DBG_PRINT(g_rtcore.c_str());
    rtcInit(g_rtcore.c_str());

    g_numThreads = embree::TaskScheduler::getNumThreads();
    DBG_PRINT(g_numThreads);

    std::cout << "RTC INIT DONE" << std::endl << std::flush;

    //TaskScheduler::create(g_numThreads);
    //DBG_PRINT(embree::TaskScheduler::getNumThreads());

#if defined(__MIC__)
    RayStreamLogger::rayStreamLogger.deactivate();
#endif
    std::string geometryFileName = g_binaries_path + "geometry.bin";
    std::string rayStreamFileName = g_binaries_path + "ray16.bin";
    std::string rayStreamVerifyFileName = g_binaries_path + "ray16_verify.bin";

    /* load geometry file */
    std::cout << "loading geometry data..." << std::flush;    
    void *g = loadGeometryData(geometryFileName);
    std::cout <<  "done" << std::endl << std::flush;
    
    /* load ray stream data */
    std::cout << "loading ray stream data..." << std::flush;    
    size_t numLogRayStreamElements = 0;
    RayStreamLogger::LogRay16 *r = (RayStreamLogger::LogRay16 *)loadRayStreamData(rayStreamFileName, numLogRayStreamElements);
    std::cout <<  "done" << std::endl << std::flush;

    RayStreamLogger::LogRay16 *verify = NULL;
    if (g_check)
      {
	std::cout << "loading ray stream verify data..." << std::flush;    
	size_t numElements = 0;
	verify = (RayStreamLogger::LogRay16 *)loadRayStreamData(rayStreamVerifyFileName, numElements );
	std::cout <<  "done" << std::endl << std::flush;
	if (numElements != numLogRayStreamElements)
	  FATAL("numElements != numLogRayStreamElements");
      }

    /* analyse ray stream data */
    std::cout << "analyse ray stream:" << std::endl << std::flush;    
    RayStreamStats stats = analyseRayStreamData(r,numLogRayStreamElements);
    std::cout << stats << std::endl;

    /* transfer geometry data */
    std::cout << "transfering geometry data:" << std::endl << std::flush;
    RTCScene scene = transferGeometryData((char*)g);

    /* retrace ray packets */
    DBG_PRINT( TaskScheduler::getNumThreads() );
    DBG_PRINT( g_numThreads );

    std::cout << "Retracing logged rays:" << std::flush;
    double avg_time = 0;
    double mrays_sec = 0;
    for (size_t i=0;i<g_frames;i++)
      {
	double dt = getSeconds();
	g_counter = 0;
	g_rays_traced = 0;
	launch_retrace_loop(scene,r,verify,numLogRayStreamElements,g_check,g_numThreads);
	dt = getSeconds()-dt;
	mrays_sec += (double)g_rays_traced / dt / 1000000.;
#if 0
	std::cout << "frame " << i << " => time " << 1000. * dt << " " << 1. / dt << " fps " << "ms " << stats.numTotalRays / dt / 1000000. << " mrays/sec" << std::endl;
#endif
      }
    std::cout << "avg. mrays/sec = " << mrays_sec / (double)g_frames << std::endl;

    /* done */
    rtcExit();
    return 0;
  }
}

int main(int argc, char** argv)
{
  try {
    return embree::main(argc, argv);
  }
  catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << std::endl;
    return 1;
  }
  catch (...) {
    std::cout << "Error: unknown exception caught." << std::endl;
    return 1;
  }
}
