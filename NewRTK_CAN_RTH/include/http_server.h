#ifndef _HTTP_SERVER_H_
#define _HTTP_SERVER_H_
#include "cc.h"




/*
*********************************************************************************************************
*                                                 MACRO'S
*********************************************************************************************************
*/



/*
*********************************************************************************************************
*                                           FUNCTION PROTOTYPES
*********************************************************************************************************
*/

void Http_Server_Init(void);
void HttpTask(void *arg);
/*
********************************************************************************************************
*                                             MODULE END
*********************************************************************************************************
*/

#endif /* HTTP_SERVER_H */


