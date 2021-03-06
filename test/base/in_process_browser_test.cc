// Copyright (c) 2013 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/test/base/in_process_browser_test.h"

#include <algorithm>
#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/debug/leak_annotations.h"
#include "base/files/file_path.h"
#include "base/file_util.h"
#include "base/lazy_instance.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/test_file_util.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_browser_thread.h"
#include "content/public/test/test_launcher.h"
#include "content/public/test/test_navigation_observer.h"
#include "content/public/test/test_utils.h"
#include "net/test/spawned_test_server/spawned_test_server.h"
#include "xwalk/runtime/browser/xwalk_runner.h"
#include "xwalk/runtime/common/xwalk_paths.h"
#include "xwalk/runtime/common/xwalk_switches.h"
#include "xwalk/runtime/renderer/xwalk_content_renderer_client.h"
#include "xwalk/test/base/xwalk_test_suite.h"
#include "xwalk/test/base/xwalk_test_utils.h"

#if defined(OS_TIZEN)
#include "xwalk/runtime/renderer/tizen/xwalk_content_renderer_client_tizen.h"
#endif

using xwalk::Runtime;
using xwalk::XWalkContentRendererClient;
using xwalk::XWalkRunner;
using xwalk::NativeAppWindow;

namespace {

// Used when running in single-process mode.
#if defined(OS_TIZEN)
base::LazyInstance<xwalk::XWalkContentRendererClientTizen>::Leaky
        g_xwalk_content_renderer_client = LAZY_INSTANCE_INITIALIZER;
#else
base::LazyInstance<XWalkContentRendererClient>::Leaky
        g_xwalk_content_renderer_client = LAZY_INSTANCE_INITIALIZER;
#endif
// Return a CommandLine object that is used to relaunch the browser_test
// binary as a browser process.
base::CommandLine GetCommandLineForRelaunch() {
  base::CommandLine new_command_line(
      base::CommandLine::ForCurrentProcess()->GetProgram());
  CommandLine::SwitchMap switches =
      CommandLine::ForCurrentProcess()->GetSwitches();
  new_command_line.AppendSwitch(content::kLaunchAsBrowser);

  for (auto iter = switches.begin(); iter != switches.end(); ++iter) {
    new_command_line.AppendSwitchNative((*iter).first, (*iter).second);
  }
  return new_command_line;
}

}  // namespace

InProcessBrowserTest::InProcessBrowserTest() {
  CreateTestServer(base::FilePath(FILE_PATH_LITERAL("xwalk/test/data")));
}

InProcessBrowserTest::~InProcessBrowserTest() {
}

void InProcessBrowserTest::SetUp() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  // Allow subclasses to change the command line before running any tests.
  SetUpCommandLine(command_line);
  // Add command line arguments that are used by all InProcessBrowserTests.
  xwalk_test_utils::PrepareBrowserCommandLineForTests(command_line);

  // Single-process mode is not set in BrowserMain, so process it explicitly,
  // and set up renderer.
  if (command_line->HasSwitch(switches::kSingleProcess)) {
    content::SetRendererClientForTesting(
        &g_xwalk_content_renderer_client.Get());
  }

  BrowserTestBase::SetUp();
}

Runtime* InProcessBrowserTest::CreateRuntime(
    const GURL& url, const NativeAppWindow::CreateParams& params) {
  Runtime* runtime = Runtime::Create(
      XWalkRunner::GetInstance()->browser_context());
  runtime->set_observer(this);
  runtimes_.push_back(runtime);
  runtime->LoadURL(url);
  content::WaitForLoadStop(runtime->web_contents());
  runtime->set_ui_delegate(
      xwalk::DefaultRuntimeUIDelegate::Create(runtime, params));
  runtime->Show();
  return runtime;
}

void InProcessBrowserTest::RunTestOnMainThreadLoop() {
  // Pump startup related events.
  content::RunAllPendingInMessageLoop();

  SetUpOnMainThread();

  if (!HasFatalFailure())
    RunTestOnMainThread();

  // Invoke cleanup and quit even if there are failures. This is similar to
  // gtest in that it invokes TearDown even if Setup fails.
  ProperMainThreadCleanup();

  CloseAll();
}

void InProcessBrowserTest::OnNewRuntimeAdded(Runtime* runtime) {
  DCHECK(runtime);
  runtimes_.push_back(runtime);
  runtime->set_observer(this);
  runtime->set_ui_delegate(
      xwalk::DefaultRuntimeUIDelegate::Create(runtime));
  runtime->Show();
}

void InProcessBrowserTest::OnRuntimeClosed(Runtime* runtime) {
  DCHECK(runtime);
  auto it = std::find(runtimes_.begin(), runtimes_.end(), runtime);
  DCHECK(it != runtimes_.end());
  runtimes_.erase(it);

  if (runtimes_.empty())
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::MessageLoop::QuitClosure());
}

void InProcessBrowserTest::CloseAll() {
  if (runtimes_.empty())
    return;

  RuntimeList to_be_closed(runtimes_.get());
  for (Runtime* runtime : to_be_closed)
    runtime->Close();
  // Wait until all windows are closed.
  content::RunAllPendingInMessageLoop();
  DCHECK(runtimes_.empty()) << runtimes_.size();
}

bool InProcessBrowserTest::CreateDataPathDir() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  base::FilePath data_path_dir =
      command_line->GetSwitchValuePath(switches::kXWalkDataPath);
  if (data_path_dir.empty()) {
    if (temp_data_path_dir_.CreateUniqueTempDir() &&
        temp_data_path_dir_.IsValid()) {
      data_path_dir = temp_data_path_dir_.path();
    } else {
      LOG(ERROR) << "Could not create temporary data directory \""
                 << temp_data_path_dir_.path().value() << "\".";
      return false;
    }
  }
  return xwalk_test_utils::OverrideDataPathDir(data_path_dir);
}

