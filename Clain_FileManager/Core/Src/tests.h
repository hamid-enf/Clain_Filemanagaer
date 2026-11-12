/*
 * tests.h
 *
 *  Created on: Sep 4, 2026
 *      Author: HAMSHAHRI
 */

#ifndef SRC_TESTS_H_
#define SRC_TESTS_H_


#include "defines.h"
#include "printf_redirect.h"


void Generate_number(){
	printf("Generate_number \n");
	for(int i = 0; i< size_data;i++){
		dataw[i] = i*10;
	}
}

void ReadSdram(){
	printf("ReadSdram \n");
	uint32_t *p = (uint32_t*)SdRamBaseAddress;
	for(int i = 0; i< size_data;i++){
		datar[i] = p[i];

		printf("datar[%d] :%1u \n",i,datar[i]);
	}
}








#endif /* SRC_TESTS_H_ */
