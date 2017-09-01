/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod REST in Pawn Extension
 * Copyright 2017 Erik Minekus
 * =============================================================================
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "curlapi.h"
#include "curlthread.h"

static size_t ReadRequestBody(void *body, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	struct HTTPRequest *request = (struct HTTPRequest *)userdata;
	size_t to_copy = (request->size - request->pos < total) ? request->size - request->pos : total;

	memcpy(body, &(request->body[request->pos]), to_copy);
	request->pos += to_copy;

	return to_copy;
}

static size_t WriteResponseBody(void *body, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	struct HTTPResponse *response = (struct HTTPResponse *)userdata;

	response->body = (char *)realloc(response->body, response->size + total + 1);
	if (response->body == NULL)
	{
		return 0;
	}

	memcpy(&(response->body[response->size]), body, total);
	response->size += total;
	response->body[response->size] = '\0';

	return total;
}

static size_t ReceiveResponseHeader(char *buffer, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	struct HTTPResponse *response = (struct HTTPResponse *)userdata;

	char *name = strtok(buffer, ":");
	char *value = strtok(NULL, "\n");
	if (value == NULL)
	{
		return total;
	}

	/* Trim leading space */
	value++;

	ke::AString vstr(value);
	response->headers.replace(name, ke::Move(vstr));

	return total;
}

void HTTPRequestThread::RunThread(IThreadHandle *pHandle)
{
	CURL *curl = curl_easy_init();
	if (curl == NULL)
	{
		forwards->ReleaseForward(this->forward);

		smutils->LogError(myself, "Could not initialize cURL session.");
		return;
	}

	char caBundlePath[PLATFORM_MAX_PATH];
	smutils->BuildPath(Path_SM, caBundlePath, sizeof(caBundlePath), SM_RIPEXT_CA_BUNDLE_PATH);

	char error[CURL_ERROR_SIZE] = {'\0'};
	const ke::AString url = this->client->BuildURL(this->request.endpoint);

	struct curl_slist *headers = this->client->BuildHeaders(this->request);
	struct HTTPResponse response;

	if (this->request.method.compare("POST") == 0)
	{
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
	}
	else if (this->request.method.compare("PUT") == 0)
	{
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	}
	else if (this->request.method.compare("PATCH") == 0)
	{
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, this->request.method.chars());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
	}
	else if (this->request.method.compare("DELETE") == 0)
	{
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, this->request.method.chars());
	}

	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &ReceiveResponseHeader);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_READDATA, &this->request);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, &ReadRequestBody);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_URL, url.chars());
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteResponseBody);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK)
	{
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);
		free(this->request.body);
		forwards->ReleaseForward(this->forward);

		smutils->LogError(myself, "HTTP request failed: %s", error);
		return;
	}

	response.data = json_loads(response.body, 0, NULL);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	free(this->request.body);

	g_RipExt.AddCallbackToQueue(HTTPRequestCallback(this->forward, response, this->value));
}
