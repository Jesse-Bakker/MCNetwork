#include "jobhandling.h"
#include "debug.h"

JobManager::JobManager(std::shared_ptr<ParameterStorage> parameterStorage) : parameterStorage(parameterStorage){
    DEBUG_FUNC_START

    std::string dataFileName = parameterStorage->workingDirecotry+ "data.hdf5";

    voltageScanPoints =    parameterStorage->parameters.at("voltageScanPoints");
    electrodeNumber   =int(parameterStorage->electrodes.size());

    //setup systems
    systems.push_back(new System(parameterStorage));
    systems[0]->initilizeMatrices();
    if(! parameterStorage->makeNewDevice){
        systems[0]->loadDevice();
    }
    else{
        systems[0]->createRandomNewDevice();
    }
    systems[0]->getReadyForRun();
    for(int i=1; i < parameterStorage->parameters.at("threads"); i++){
        systems.push_back(new System(*systems[0]));
    }

    DEBUG_FUNC_END
}


std::pair<std::vector<double>,std::vector<double>> const JobManager::runControlVoltagesSetup(std::vector<double> const & voltages){
    DEBUG_FUNC_START

    //make Jobs
    for (size_t i = 0; i < voltageScanPoints; i++){
        for (size_t j = 0; j < voltageScanPoints; j++){
            jobs.push_back(Job(i*voltageScanPoints + j));
            jobs[i*voltageScanPoints + j].equilSteps   = parameterStorage->parameters.at("equilSteps");
            jobs[i*voltageScanPoints + j].totalSteps   = parameterStorage->parameters.at("calcCurrentSteps");
            jobs[i*voltageScanPoints + j].stepsPerTask = parameterStorage->parameters.at("calcCurrentSteps")/jobs[i*voltageScanPoints + j].tasksPerJob;
            jobs[i*voltageScanPoints + j].tasksToGo    = jobs[i*voltageScanPoints + j].tasksPerJob;
            jobs[i*voltageScanPoints + j].voltages     = voltages;
            jobs[i*voltageScanPoints + j].voltages[parameterStorage->parameters.at("outputElectrode")] = 0;
            jobs[i*voltageScanPoints + j].voltages[parameterStorage->parameters.at("inputElectrode1")] = parameterStorage->inputVoltages[i];
            jobs[i*voltageScanPoints + j].voltages[parameterStorage->parameters.at("inputElectrode2")] = parameterStorage->inputVoltages[j];
        }
    }
    


    if (parameterStorage->parameters.at("threads") > 1){
        std::vector<std::thread> threads;
        for(int k=0; k < parameterStorage->parameters.at("threads"); k++){
            threads.push_back(std::thread(&JobManager::handleJobList, std::ref(jobs), systems[k], std::ref(jobSearchMutex)));
        }
        for(int k=0; k < parameterStorage->parameters.at("threads"); k++){
            threads[k].join();
        }
        threads.clear();
    }
    else{
        handleJobList(jobs, systems[0], jobSearchMutex);
    }



    std::vector<double> currents     (voltageScanPoints*voltageScanPoints);
    std::vector<double> currentUncert(voltageScanPoints*voltageScanPoints);

    for (size_t k = 0; k < voltageScanPoints*voltageScanPoints; k++){
        currents[k]      = jobs[k].resultCurrent;
        currentUncert[k] = jobs[k].resultCurrentUncert;
    }

    jobs.clear();

    return std::pair<std::vector<double>,std::vector<double>>(currents,currentUncert);

    DEBUG_FUNC_END
}

void JobManager::handleJobList(std::vector<Job> & jobs, System * system, std::mutex & searchMutex){
    Job *  bestJob;
    double mostTasks = 0;

    while(true){
        //search for job
        mostTasks = 0;
        searchMutex.lock();
        for(auto & job: jobs){
            //calc work to do
            if (job.tasksToGo/(job.threadNumber+1) > mostTasks){
                mostTasks = job.tasksToGo/(job.threadNumber+1);
                bestJob = & job;
            }            
        }
        //check if job is worth to join
        if(mostTasks > 1){
            //join work on job
            // std::cout<<"joining job: "<<bestJob->ID<<std::endl;
            bestJob->threadNumber += 1;
            bestJob->tasksToGo    -= 1;
            //check if job was started already by another thread
            if (bestJob->tasksToGo + 1 == bestJob->tasksPerJob){
                //first thread in job, so potential and equilibrium state have to be computed. therefor first lock the job.
                bestJob->jobMutex->lock();
                searchMutex.unlock(); //job is locked, so other jobs can start searching

                //calc new potential
                system->updatePotential(bestJob->voltages);
                bestJob->potential       = system->getPotential();

                //calc equil steps
                system->resetStoredStates();
                system->run(bestJob->equilSteps);
                bestJob->equilOccupation = system->getOccupation();

                bestJob->jobMutex->unlock();
            }
            else{
                //job was already started by another thread
                searchMutex.unlock(); //job dont has to be locked, so other jobs can start searching
                //check if job starting calculations are done, otherwise wait
                bestJob->jobMutex->lock();
                bestJob->jobMutex->unlock();

                //copy shit here
                system->updateOccupationAndPotential  (bestJob->equilOccupation, bestJob->potential);
            }

            //first task
            system->resetStoredStates();
            system->reset();
            system->run(bestJob->stepsPerTask);
            bestJob->resultCurrent      +=*(system->outputCurrentCounter)/system->time;
            bestJob->resultCurrentUncert+=std::pow(*(system->outputCurrentCounter)/system->time,2); //storing current**2 here
            system->reset();

            //run tasks
            while(true){
                searchMutex.lock();
                if (bestJob->tasksToGo > 0){
                    bestJob->tasksToGo    -= 1;
                    searchMutex.unlock();

                    system->run(bestJob->stepsPerTask);
                    bestJob->resultCurrent      +=*(system->outputCurrentCounter)/system->time;
                    bestJob->resultCurrentUncert+=std::pow(*(system->outputCurrentCounter)/system->time,2);

                    system->reset();
                }
                else{
                    if(bestJob->threadNumber == 1){
                        //finish job
                        bestJob->resultCurrentUncert =  std::sqrt(bestJob->resultCurrentUncert - bestJob->resultCurrent*bestJob->resultCurrent/bestJob->tasksPerJob)/bestJob->tasksPerJob;
                        bestJob->resultCurrent       /= bestJob->tasksPerJob;

                        std:cout<<"current: "<<bestJob->resultCurrent<<" +- "<<bestJob->resultCurrentUncert;
                        std::cout<<" voltages: ";
                        for (size_t i = 0; i < bestJob->voltages.size(); i++){
                            std::cout<<i<<": "<<bestJob->voltages[i]<<" ";
                        }
                        std::cout<<std::endl;
                    }
                    bestJob->threadNumber -= 1;
                    searchMutex.unlock();
                    break;
                }
            }

            // !!!!!!!!!!! reset sytem shares states !!!!!!!!

            // system->copyPotential(potential);
            // system->copyState    (bestJob->equilState);

        }
        else{
            // std::cout<<"work done for me!"<<std::endl;
            searchMutex.unlock();
            return;
        }
    }
}