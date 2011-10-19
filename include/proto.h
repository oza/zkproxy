/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef PROTO_H_
#define PROTO_H_

#ifdef __cplusplus
extern "C" {
#endif

#if 0
static const int NOTIFY_OP=0;
static const int CREATE_OP=1;
static const int DELETE_OP=2;
static const int EXISTS_OP=3;
static const int GETDATA_OP=4;
static const int SETDATA_OP=5;
static const int GETACL_OP=6;
static const int SETACL_OP=7;
static const int GETCHILDREN_OP=8;
static const int SYNC_OP=9;
static const int PING_OP=11;
static const int GETCHILDREN2_OP=12;
static const int CLOSE_OP=-11;
static const int SETAUTH_OP=100;
static const int SETWATCHES_OP=101;
#else
#define NOTIFY_OP 0
#define CREATE_OP 1
#define DELETE_OP 2
#define EXISTS_OP 3
#define GETDATA_OP 4
#define SETDATA_OP 5
#define GETACL_OP 6
#define SETACL_OP 7
#define GETCHILDREN_OP 8
#define SYNC_OP 9
#define PING_OP 11
#define GETCHILDREN2_OP 12
#define CLOSE_OP -11
#define SETAUTH_OP 100
#define SETWATCHES_OP 101

#endif

#ifdef __cplusplus
}
#endif

#endif /*PROTO_H_*/
