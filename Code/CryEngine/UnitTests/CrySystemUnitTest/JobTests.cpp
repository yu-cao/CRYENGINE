// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved. 

#include <UnitTest.h>
#include <CryThreading/IJobManager.h>
#include <CryThreading/IJobManager_JobDelegator.h>
#include <CryThreading/IThreadManager.h>
#include <Timer.h>
#include <System.h> // CrySystem module's System.h
#include <vector>
#include <memory>
#include <atomic>

class CTestJobHost
{
public:
	void JobEntry(int x)
	{
		m_value = x;
		CrySleep(1);
		m_isDone = true;
	}

	int GetValue() const { return m_value; }
	bool IsDoneCalculating() const { return m_isDone; }

private:
	volatile bool m_isDone = false;
	int m_value = 0;
};

DECLARE_JOB("TestJob", TTestJob, CTestJobHost::JobEntry);

class CJobSystemTest : public ::testing::Test
{
protected:
	virtual void SetUp() override
	{
		gEnv->pJobManager = GetJobManagerInterface();
		gEnv->pTimer = new CTimer();

		auto pseudoProfilerCallback = [](class CFrameProfilerSection* pSection) {};
		gEnv->callbackStartSection = pseudoProfilerCallback;
		gEnv->callbackEndSection = pseudoProfilerCallback;
		gEnv->pThreadManager = CreateThreadManager();
		gEnv->pJobManager->Init(8);
	}

	virtual void TearDown() override
	{
		delete gEnv->pTimer;
		delete gEnv->pThreadManager;
	}
};

TEST_F(CJobSystemTest, MemberFunctionJobSimple)
{
	CTestJobHost host;
	TTestJob job(42);
	job.SetClassInstance(&host);
	job.SetPriorityLevel(JobManager::eStreamPriority);
	job.Run();
	while (!host.IsDoneCalculating()) {}
	REQUIRE(host.GetValue() == 42);
}

TEST_F(CJobSystemTest, MemberFunctionJobMultiple)
{
	JobManager::SJobState jobState;

	std::vector<std::unique_ptr<CTestJobHost>> jobObjects;
	for (int i = 0; i < 100; i++)
	{
		std::unique_ptr<CTestJobHost> host = stl::make_unique<CTestJobHost>();
		TTestJob job(i);
		job.SetClassInstance(host.get());
		job.RegisterJobState(&jobState);
		job.SetPriorityLevel(JobManager::eStreamPriority);
		job.Run();
		jobObjects.push_back(std::move(host));
	}

	jobState.Wait();
	for (int i = 0; i < 100; i++)
	{
		REQUIRE(jobObjects[i]->GetValue() == i);
	}
}

// Tests whether the job system is able to cache the copy of parameters passed to the job on creation.
// The job system must not directly forward l-value references to the callback, it must store a copy.
TEST_F(CJobSystemTest, MemberFunctionJobLifeTime)
{
	struct SLifeTimeTestHost
	{
		void JobEntry(const string& str)
		{
			m_value = str;
			m_isDone = true;
		}
		volatile bool m_isDone = false;
		string m_value;
	};

	static SLifeTimeTestHost gJobSystemLifeTimeTestHost;

	DECLARE_JOB("SLifeTimeTestHost", SLifeTimeTestHostJob, SLifeTimeTestHost::JobEntry);

	auto GetLifeTimeTestHostJob = []
	{
		string str = "abc";
		SLifeTimeTestHostJob* pJob = new SLifeTimeTestHostJob(str);
		return pJob;
	};

	gJobSystemLifeTimeTestHost = {}; //reset

	SLifeTimeTestHostJob* pJob = GetLifeTimeTestHostJob();
	pJob->SetClassInstance(&gJobSystemLifeTimeTestHost);
	pJob->Run();
	while (!gJobSystemLifeTimeTestHost.m_isDone) {}
	REQUIRE(gJobSystemLifeTimeTestHost.m_value == "abc");
	delete pJob;
}


static string gFreeFunctionLifetimeTestStringResult;

void SFreeFunctionLifeTimeTestCallback(const string& str)
{
	gFreeFunctionLifetimeTestStringResult = str;
}

DECLARE_JOB("SFreeFunctionLifeTimeTestJob", SFreeFunctionLifeTimeTestJob, SFreeFunctionLifeTimeTestCallback);

TEST_F(CJobSystemTest, FreeFunctionJobLifeTime)
{
	gFreeFunctionLifetimeTestStringResult = string();

	auto GetLifeTimeTestHostJob = []
	{
		string str = "abc";
		SFreeFunctionLifeTimeTestJob* pJob = new SFreeFunctionLifeTimeTestJob(str);
		return pJob;
	};

	SFreeFunctionLifeTimeTestJob* pJob = GetLifeTimeTestHostJob();
	JobManager::SJobState jobState;
	pJob->RegisterJobState(&jobState);
	pJob->Run();
	jobState.Wait();
	REQUIRE(gFreeFunctionLifetimeTestStringResult == "abc");
	delete pJob;
}

TEST_F(CJobSystemTest, MoveConstructor)
{
	CTestJobHost host;
	TTestJob job(42);
	job.SetClassInstance(&host);
	job.SetPriorityLevel(JobManager::eStreamPriority);
	TTestJob job2 = std::move(job);
	job2.Run();
	while (!host.IsDoneCalculating()) {}
	REQUIRE(host.GetValue() == 42);
}

TEST_F(CJobSystemTest, LambdaJobOld)
{
	std::atomic<int> v { 0 };
	JobManager::SJobState jobState;
	for (int i = 0; i < 100; i++)
	{
		gEnv->pJobManager->AddLambdaJob("ExampleJob1", [&v]
		{
			++v;
		}, JobManager::eRegularPriority, &jobState);
	}
	jobState.Wait();
	REQUIRE(static_cast<int>(v) == 100);
}


DECLARE_LAMBDA_JOB("TestLambdaJob", TTestLambdaJob);
DECLARE_LAMBDA_JOB("TestLambdaJob2", TTestLambdaJob2, void(int));


struct SDestructorDetector
{
	static bool isCalled;

	SDestructorDetector() = default;
	SDestructorDetector(const SDestructorDetector&) = default;
	SDestructorDetector(SDestructorDetector&&) = default;
	SDestructorDetector& operator=(const SDestructorDetector&) = default;
	SDestructorDetector& operator=(SDestructorDetector&&) = default;

	~SDestructorDetector()
	{
		isCalled = true;
	}
};

bool SDestructorDetector::isCalled = false;

TEST_F(CJobSystemTest, LambdaJobNew)
{
	int v = 0;

	SDestructorDetector destructorDetector;
	JobManager::SJobState jobState;

	// We test that the lambda is properly called and the lambda and all the captures are destructed.
	{
		TTestLambdaJob job = [&v, destructorDetector]
		{
			v = 20;
		};
		job.SetPriorityLevel(JobManager::eRegularPriority);
		job.RegisterJobState(&jobState);
		SDestructorDetector::isCalled = false;
		job.Run();
		REQUIRE(!SDestructorDetector::isCalled);
		jobState.Wait();
		REQUIRE(SDestructorDetector::isCalled);
		REQUIRE(v == 20);
	}

	{
		TTestLambdaJob2 job([&v](int x)
		{
			v = x;
		}, 23);
		job.SetPriorityLevel(JobManager::eRegularPriority);
		job.RegisterJobState(&jobState);
		job.Run();
		jobState.Wait();
		REQUIRE(v == 23);
	}
}
