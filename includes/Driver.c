/*--------------------------------------------------------------
* EQ8 - Driver
* Author: Jim Leipold
* Email: james.leipold@hotmail.com
* Created on: 11/02/2020
* Last modifiied on: 21/07/2020
* 
* This library contains common, mount specific commands.
*-------------------------------------------------------------*/

#define VERSION_DRIVER 2.14
#define STEPS_PER_REV 0xA9EC00
#define MAX_INPUT 128

#include "Comms.c"
#include <math.h>

unsigned long initial_X_position = -1;
unsigned long initial_Y_position = -1;
long max_X_positon;
long min_X_positon;
long max_Y_positon;
long min_Y_positon;

int port;
/* *
* Determines if a key has been pressed, non blocking
* Not perfect and should be replaced but works for now
* This is used to return to main from functions that run indefinitely
* */
int kbhit()
{
    struct timeval tv;
    fd_set fds;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds); //STDIN_FILENO is 0
    select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    return FD_ISSET(STDIN_FILENO, &fds);
}

/* *
 * Function to shutdown port connection.
 * Returns port ID on success, -1 on failure.
 * Not shutting down can (rarely) cause problems on some computers
 * */
int shutdown_Controller(int port)
{
    return close(port); /* Close the serial port */
}

/* *
 * Positional data sent from the mount is little endian:    0xefcdab
 * This function reformats so commands can be written by user as 
 * 0xabcdef and sent as 0xefcdab.
 * data_in is the unformatted command and must include the 
 * character command flag and channel eg. if setting target position
 * data_in  = "S1abcdef"
 * data_out = "S1efcdab" 
 * */
int convert_Command(char data_in[10], char data_out[8])
{
    if (verbose)
        printf("DRIVER_DEBUG(convert_Command):data_in\t%s\n", data_in);

    size_t length = strlen(data_in);
    if (length < 7 && verbose)
    {
        printf("DRIVER_DEBUG(convert_Command): Convert stringlen error, length: %zu\n", length);
        return -1;
    }

    char data_out_temp[8] = {data_in[0],
                             data_in[1],
                             data_in[6],
                             data_in[7],
                             data_in[4],
                             data_in[5],
                             data_in[2],
                             data_in[3]};
    strcpy(data_out, data_out_temp);
    if (verbose)
    {

        printf("DRIVER_DEBUG(convert_Command):data_out\t%s\n", data_out);
    }
    return 1;
}

/* *
 * Positional data sent from the mount is little endian:    0xefcdab
 * This function reformats for readability to:          0xabcdef
 * Note that all responses from mount are RC terminated, hence 8 Bytes
 * This function takes the data array and an output buffer as arguments
 * If output buffer == 0, fuction prints conversion only
 * To avoid formatting issues, the trailing RC from the mount is
 * replaced with '\0'
 * data_in = "=efcdab\r"
 * data_out = "=abcdef\0"
 * */
int convert_Response(char data_in[8], char data_out[8])
{
    unsigned long length = strlen(data_in);
    if (length < 7 && verbose)
    {
        printf("DRIVER_DEBUG(convert_Response): Convert stringlen error, length: %lu\n", length);
        return -1;
    }

    char data_out_temp[8] = {data_in[0],
                             data_in[5],
                             data_in[6],
                             data_in[3],
                             data_in[4],
                             data_in[1],
                             data_in[2],
                             '\0'};
    if (data_out != 0)
        strcpy(data_out, data_out_temp);
    else if (verbose)
        printf("DRIVER_DEBUG(convert_Response)Converted: %s\n", data_out_temp);
    return 1;
}

/* *
 * Function to interpret response from mount
 * Determines if response is error (and reports error type),
 * or success and presents data.
 * Prints little endian data as big endian.
 * Returns flag.
 * */
int parse_Response(struct response *response)
{
    int flag = (*response).flag;
    char data[9];
    int success = flag;
    strcpy(data, (*response).data);
    printf("Response: %s\n", data);
    if (flag == -1 && verbose)
    {
        printf("DRIVER_DEBUG(parse_Response): Read Error");
        success = -1;
    }

    else if (flag == 1)
    {
        success = 1;
        if (data[0] == '!')
        {
            printf("Mount Error: ");
            success = 0;
            if (data[1] == '0')
                printf("Unknown Command\n");
            if (data[1] == '1')
                printf("Command Length\n");
            if (data[1] == '2')
                printf("Motor Not Stopped\n");
            if (data[1] == '3')
                printf("Invalid Characer\n");
            if (data[1] == '4')
                printf("Not Initialised\n");
            if (data[1] == '5')
                printf("Driver Sleeping\n");
        }
        else if (strlen(data) == 8)
            convert_Response(data, 0);
    }
    return success;
}

/**
 * Function to send a command to the mount and recieve
 * its response. Retries TX upon mount error 10 times..E
 * Returns a pointer to the structure containing flag & data
 * */
struct response *send_Command(char command[])
{
    int retries = 9;
    struct response *resp;
    char writebuffer[strlen(command) + 2]; // Create buffer with length of the command +2 for the leading ":" and trailing RC
    strcpy(writebuffer, ":");
    strcat(writebuffer, command);
    strcat(writebuffer, "\r");
    //if (!wait_for_response)
    //goto EXIT
SEND:
    TX(port, writebuffer);
    usleep(30000); // this gives the mount time to respond
    resp = RX(port);
    if ((((*resp).data[0] == '!' && (*resp).data[1] != '0') || (*resp).flag == -1) && retries < 10) // if mount responds with error or does not respond, and there have been less than 10 retries
    {
        if (verbose && retries)
            printf("DRIVER_DEBUG(send_Command): Retry #%i\n", retries);
        usleep(10000);
        retries++;
        goto SEND; // retry command
    }
    //EXIT:
    return resp;
}

/* *
 * Function to get status of mount.
 * Returns 1 if axis is moving,
 * 0 if axis is stationary 
 * */
int get_Status(int channel)
{
    char command[3] = {'f', '1'};
    if (channel == 2)
        command[1] = '2';
    struct response *this_Response = send_Command(command);
    if (verbose)
        printf("Status: %s\n", (*this_Response).data);

    return atoi(&(this_Response->data[2]));
}

int stop_channel(int channel)
{
    char command[3] = {'K', '1'};
    if (channel == 1)
        command[1] = '1';
    else if (channel == 2)
        command[1] = '2';
    else if (channel == 3)
        command[1] = '3';
    else
        return -1;

    return send_Command(command)->flag;
}

/**
 * Function to return the target position of the mount in order
 * to turn it a given number of degrees
 **/
unsigned long angle_to_argument(int channel, double angle)
{
    char curr_Pos_String[8];
    if (channel == 1)
        convert_Response((*send_Command("j1")).data, curr_Pos_String);
    else if (channel == 2)
        convert_Response((*send_Command("j2")).data, curr_Pos_String);
    else
        return -1;
    curr_Pos_String[0] = '0'; //strips the leading '='
    unsigned long curr_Pos = strtol(curr_Pos_String, NULL, 16);
    unsigned long target_Pos = curr_Pos + roundf(angle * (STEPS_PER_REV / 360));
    target_Pos = target_Pos % 0xA9EC00;
    if (verbose)
    {
        printf("DRIVER_DEBUG(angle_to_argument): Current position: %06lX\n", curr_Pos);
        printf("DRIVER_DEBUG(angle_to_argument): Target position: %06lX\n", target_Pos);
    }

    return target_Pos;
}

char *lu_to_string(unsigned long input)
{
    static char output[6];
    memset(output, 0, strlen(output));
    sprintf(output, "%06lX", input);
    return output;
}

/**
 * Function to move mount to target position.
 * Channel specifies which axis
 * Target is the target encoder position as a character array
 * isFormatted specifies whether target is formatted for readability ( 0 for 0xabcdef),
 * or in the mounts format (1 for 0xefcdab), allowing for either to be used.
 * Returns 1 on success, -1 on failure, including any errors thrown by mount.
 **/
int go_to(int channel, char target[MAX_INPUT], bool isFormatted)
{
    char command[10]; //Less than 10 causes problems
    command[0] = 'S';
    stop_channel(channel);
    long target_long = strtol(target, NULL, 16);
    //printf("TARGET: %6lX\n", target_long);
    if (channel == 1)
    {
        command[1] = '1';
        if (target_long > max_Y_positon || target_long < min_Y_positon)

        {
            printf("Target exceeds boundaries");
            return -1;
        }
    }
    if (channel == 2)
    {
        command[1] = '2';
        if (target_long > max_X_positon || target_long < min_X_positon)
        {
            printf("Target exceeds boundaries");
            return -1;
        }
    }

    command[2] = '\0'; //strcat needs null terminator
    strcat(command, target);

    if (!isFormatted)
        convert_Command(command, command);
    if (channel == 1)
    {
        send_Command(command);
        send_Command("G101");
        send_Command("J1");
    }
    else if (channel == 2)
    {
        send_Command(command);
        send_Command("G201");
        send_Command("J2");
    }
    return 1;
}

/**
 * Function to move mount a relative angular distance
 * Channel specifies which axis
 * Returns 1 on success, -1 on failure, including any errors thrown by mount.
 **/
int turn(int channel, float angle)
{
    stop_channel(channel);
    long target = angle_to_argument(channel, angle);
    if (verbose)
        printf("DRIVER_DEBUG(parse_COMMAND: Target(char): %s\n", lu_to_string(target));
    return go_to(channel, lu_to_string(target), false);
}

/**
 * Function to position of axis mount
 * */
unsigned long get_Position(int channel)
{
    static char data[6];
    char command[3] = "j1";
    if (channel == 1)
        command[1] = '1';
    else if (channel == 2)
        command[1] = '2';
    else
    {
        printf("Invalid channel number");
        return '\0';
    }
    struct response *resp = send_Command(command);
    if ((*resp).flag == 2 || (*resp).flag == -1) //error
    {
        printf("Position enquiry failed\n");
        return -1;
    }
    convert_Response((*resp).data, data);
    data[0] = '0'; //strips the leading '='
    unsigned long position = strtol(data, NULL, 16);
    return position;
}
