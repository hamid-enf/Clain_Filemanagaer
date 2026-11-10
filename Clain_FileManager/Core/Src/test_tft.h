/*
 * test_tft.h
 *
 *  Created on: Sep 4, 2026
 *      Author: HAMSHAHRI
 */

#ifndef SRC_TEST_TFT_H_
#define SRC_TEST_TFT_H_

#include "defines.h"


void init_tft(){
	  ILI9341_Init();

	//==========================================================================
	//=  حالت						عرض			ارتفاع		توضیح
	//=  SCREEN_VERTICAL_1			240				320		عمودی — پیش‌فرض
	//=  SCREEN_HORIZONTAL_1		320				240		افقی
	//=  SCREEN_VERTICAL_2			240				320		عمودی برعکس
	//=  SCREEN_HORIZONTAL_2		320				240		افقی برعکس
	//===========================================================================
	  ILI9341_Set_Rotation(SCREEN_HORIZONTAL_2);

	  // پاک کردن صفحه با رنگ آبی
	  ILI9341_Fill_Screen(BLUE);
	  // نوشتن متن
	  ILI9341_Draw_Text("Hello STM32H7!", 10, 10, WHITE, 2, BLACK);
	//
}


#endif /* SRC_TEST_TFT_H_ */
